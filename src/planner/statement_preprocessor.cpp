#include "duckdb/planner/statement_preprocessor.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parser.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/pragma_function_catalog_entry.hpp"
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/parsed_data/bound_pragma_info.hpp"
#include "duckdb/function/function.hpp"

#include "duckdb/main/client_context.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/statement/set_statement.hpp"
#include "duckdb/common/enums/current_transaction_state.hpp"

namespace duckdb {

enum class PreprocessingTransactionHandling : uint8_t {
	// Not in a transaction and should wrap in an implicit BEGIN/COMMIT
	WRAP_IN_TRANSACTION,
	// Already in an active transaction — set invalidation policy to ALL_ERRORS_INVALIDATE_TRANSACTION and back to
	// STANDARD_POLICY
	SET_INVALIDATION_POLICY_TO_ALL_ERRORS,
	// No transaction handling needed (single statement, or no transaction context applies)
	NONE
};

void AddStatements(vector<unique_ptr<SQLStatement>> &body_statements,
                   const PreprocessingTransactionHandling transaction_handling,
                   vector<unique_ptr<SQLStatement>> &result_statements) {
	if (transaction_handling == PreprocessingTransactionHandling::WRAP_IN_TRANSACTION) {
		auto begin_info = make_uniq<TransactionInfo>(
		    TransactionType::BEGIN_TRANSACTION, TransactionInvalidationPolicy::ALL_ERRORS_INVALIDATE_TRANSACTION, true);
		result_statements.push_back(make_uniq<TransactionStatement>(std::move(begin_info)));
	} else if (transaction_handling == PreprocessingTransactionHandling::SET_INVALIDATION_POLICY_TO_ALL_ERRORS) {
		// Here we do a `SET current_transaction_invalidation_policy='ALL_ERRORS_INVALIDATE_TRANSACTION';`, for the
		// current transaction, to make sure multistatements/pragmas are fully transactional, and invalidate even with
		// minor errors such as binder, parser, etc.
		result_statements.push_back(make_uniq<SetVariableStatement>(
		    "current_transaction_invalidation_policy",
		    make_uniq<ConstantExpression>(Value("ALL_ERRORS_INVALIDATE_TRANSACTION")), SetScope::GLOBAL));
	}
	// insert body_statements into result_statements
	result_statements.insert(result_statements.end(), std::make_move_iterator(body_statements.begin()),
	                         std::make_move_iterator(body_statements.end()));
	if (transaction_handling == PreprocessingTransactionHandling::WRAP_IN_TRANSACTION) {
		auto commit_info = make_uniq<TransactionInfo>(
		    TransactionType::COMMIT, TransactionInvalidationPolicy::ALL_ERRORS_INVALIDATE_TRANSACTION, true);
		result_statements.push_back(make_uniq<TransactionStatement>(std::move(commit_info)));
	} else if (transaction_handling == PreprocessingTransactionHandling::SET_INVALIDATION_POLICY_TO_ALL_ERRORS) {
		result_statements.push_back(
		    make_uniq<SetVariableStatement>("current_transaction_invalidation_policy",
		                                    make_uniq<ConstantExpression>(Value("STANDARD_POLICY")), SetScope::GLOBAL));
	}
}

StatementPreprocessor::StatementPreprocessor(ClientContext &context) : context(context) {
}

PreprocessingTransactionHandling GetTransactionHandling(
    vector<unique_ptr<SQLStatement>> &body_statements, CurrentTransactionState full_transaction_state,
    const TransactionInvalidationPolicy starting_transaction_invalidation_policy, bool can_wrap = true) {
	if (body_statements.size() <= 1) {
		return PreprocessingTransactionHandling::NONE;
	}
	if (full_transaction_state == NOT_IN_ACTIVE_TRANSACTION && can_wrap) {
		return PreprocessingTransactionHandling::WRAP_IN_TRANSACTION;
	}
	if (full_transaction_state == IN_ACTIVE_TRANSACTION &&
	    starting_transaction_invalidation_policy == TransactionInvalidationPolicy::STANDARD_POLICY) {
		return PreprocessingTransactionHandling::SET_INVALIDATION_POLICY_TO_ALL_ERRORS;
	}
	return PreprocessingTransactionHandling::NONE;
}

void UnpackMultiStatement(MultiStatement &multi_statement, const CurrentTransactionState current_transaction_state,
                          vector<unique_ptr<SQLStatement>> &new_statements,
                          const TransactionInvalidationPolicy starting_transaction_invalidation_policy) {
#ifdef DEBUG // MultiStatement should not contain transaction statements
	for (auto &sub_statement : multi_statement.statements) {
		D_ASSERT(sub_statement->type != StatementType::TRANSACTION_STATEMENT);
	}
#endif
	bool has_select = false;
	for (auto &stmt : multi_statement.statements) {
		if (stmt->type == StatementType::SELECT_STATEMENT) {
			// Pivot statements have select, and we don't want to wrap those in transactions.
			has_select = true;
		}
	}
	bool can_wrap_in_transaction = !has_select;
	auto handling = GetTransactionHandling(multi_statement.statements, current_transaction_state,
	                                       starting_transaction_invalidation_policy, can_wrap_in_transaction);
	AddStatements(multi_statement.statements, handling, new_statements);
}

vector<unique_ptr<SQLStatement>> StatementPreprocessor::TryReparsePragma(unique_ptr<SQLStatement> statement) const {
	// Try reparsing
	const auto info = statement->Cast<PragmaStatement>().info->Copy();
	QueryErrorContext error_context(statement->stmt_location);
	const auto binder = Binder::CreateBinder(context);
	const auto bound_info = binder->BindPragma(*info, error_context);
	if (bound_info->function.query) {
		// Needs reparsing
		FunctionParameters parameters {bound_info->parameters, bound_info->named_parameters};
		const auto query_to_reparse = bound_info->function.query(context, parameters);
		Parser parser(context.GetParserOptions());
		parser.ParseQuery(query_to_reparse);
		return std::move(parser.statements);
	}
	vector<unique_ptr<SQLStatement>> res;
	res.push_back(std::move(statement));
	return res;
}

void StatementPreprocessor::Preprocess(ClientContextLock &lock, vector<unique_ptr<SQLStatement>> &statements,
                                       const TransactionContext &transaction_context) {
	// Quick check: do we need preprocessing at all?
	bool needs_preprocessing = false;
	for (auto &stmt : statements) {
		if (stmt->type == StatementType::PRAGMA_STATEMENT || stmt->type == StatementType::MULTI_STATEMENT) {
			needs_preprocessing = true;
			break;
		}
	}
	if (!needs_preprocessing) {
		return;
	}

	context.RunFunctionInTransactionInternal(lock, [&] { PreprocessInternal(lock, statements, transaction_context); });
}

void StatementPreprocessor::PreprocessInternal(ClientContextLock &lock, vector<unique_ptr<SQLStatement>> &statements,
                                               const TransactionContext &transaction_context) {
	const CurrentTransactionState transaction_context_state =
	    transaction_context.HasActiveTransaction() ? IN_ACTIVE_TRANSACTION : NOT_IN_ACTIVE_TRANSACTION;

	const auto starting_transaction_invalidation_policy = transaction_context.GetInvalidationPolicy();

	CurrentTransactionState chained_transaction_state = NOT_IN_ACTIVE_TRANSACTION;
	vector<unique_ptr<SQLStatement>> new_statements;
	for (idx_t i = 0; i < statements.size(); i++) {
		auto query = statements[i]->query;
		const CurrentTransactionState full_transaction_state =
		    (transaction_context_state == IN_ACTIVE_TRANSACTION || chained_transaction_state == IN_ACTIVE_TRANSACTION)
		        ? IN_ACTIVE_TRANSACTION
		        : NOT_IN_ACTIVE_TRANSACTION;

		switch (statements[i]->type) {
		case StatementType::PRAGMA_STATEMENT: {
			auto reparsed_statements = TryReparsePragma(std::move(statements[i]));
			const auto handling = GetTransactionHandling(reparsed_statements, full_transaction_state,
			                                             starting_transaction_invalidation_policy);
			AddStatements(reparsed_statements, handling, new_statements);
			break;
		}
		case StatementType::MULTI_STATEMENT: {
			auto &multi_statement = statements[i]->Cast<MultiStatement>();
			UnpackMultiStatement(multi_statement, full_transaction_state, new_statements,
			                     starting_transaction_invalidation_policy);
			break;
		}
		case StatementType::TRANSACTION_STATEMENT: {
			auto &transaction_stmt = statements[i]->Cast<TransactionStatement>();

			if (transaction_stmt.info->type == TransactionType::BEGIN_TRANSACTION) {
				new_statements.push_back(std::move(statements[i]));
				chained_transaction_state = IN_ACTIVE_TRANSACTION;
				break;
			}
			if (transaction_stmt.info->type == TransactionType::COMMIT ||
			    transaction_stmt.info->type == TransactionType::ROLLBACK) {
				chained_transaction_state = NOT_IN_ACTIVE_TRANSACTION;
			}
			new_statements.push_back(std::move(statements[i]));
			break;
		}
		default: {
			new_statements.push_back(std::move(statements[i]));
		}
		}
	}

	statements = std::move(new_statements);
}
} // namespace duckdb
