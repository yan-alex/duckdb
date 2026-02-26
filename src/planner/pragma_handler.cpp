#include "duckdb/planner/pragma_handler.hpp"
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
#include "duckdb/parser/statement/multi_statement.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"

namespace duckdb {

PragmaHandler::PragmaHandler(ClientContext &context) : context(context) {
}

void PragmaHandler::HandlePragmaAndMultiStatementsInternal(vector<unique_ptr<SQLStatement>> &statements,
                                                           bool is_in_active_transaction) {
	vector<unique_ptr<SQLStatement>> new_statements;
	for (idx_t i = 0; i < statements.size(); ++i) {
		auto stmt = std::move(statements[i]);

		switch (stmt->type) {
		case StatementType::MULTI_STATEMENT: {
			auto multi_stmt = static_cast<MultiStatement *>(stmt.get());

#ifdef DEBUG // MultiStatement cannot contain transaction statements
			for (auto &sub_statement : multi_stmt->statements) {
				D_ASSERT(sub_statement->type != StatementType::TRANSACTION_STATEMENT);
			}
#endif
			if (is_in_active_transaction) {
				// add sub-statements
				for (auto &stmt : multi_stmt->statements) {
					new_statements.push_back(std::move(stmt));
				}
			} else {
				// inject BEGIN
				auto begin_info = make_uniq<TransactionInfo>(TransactionType::BEGIN_TRANSACTION);
				new_statements.push_back(make_uniq<TransactionStatement>(std::move(begin_info)));

				// add sub-statements
				for (auto &stmt : multi_stmt->statements) {
					new_statements.push_back(std::move(stmt));
				}

				// inject COMMIT
				auto commit_info = make_uniq<TransactionInfo>(TransactionType::COMMIT);
				new_statements.push_back(make_uniq<TransactionStatement>(std::move(commit_info)));
			}
			break;
		}
		case StatementType::TRANSACTION_STATEMENT: {
			auto transaction_stmt = static_cast<TransactionStatement *>(stmt.get());
			if (transaction_stmt->info->type == TransactionType::BEGIN_TRANSACTION) {
				is_in_active_transaction = true;
			} else if (transaction_stmt->info->type == TransactionType::COMMIT ||
			           transaction_stmt->info->type == TransactionType::ROLLBACK) {
				is_in_active_transaction = false;
			}
			new_statements.push_back(std::move(stmt));
			break;
		}
		case StatementType::PRAGMA_STATEMENT: {
			// PRAGMA statement: check if we need to replace it by a new set of statements
			PragmaHandler handler(context);
			string new_query;
			bool expanded = handler.HandlePragma(*stmt, new_query);
			if (expanded) {
				// this PRAGMA statement gets replaced by a new query string
				// push the new query string through the parser again and add it to the transformer
				Parser parser(context.GetParserOptions());
				parser.ParseQuery(new_query);
				// insert the new statements and remove the old statement
				for (idx_t j = 0; j < parser.statements.size(); j++) {
					new_statements.push_back(std::move(parser.statements[j]));
				}
				continue;
			}
			break;
		}
		default: {
			throw InternalException("PragmaHandler::HandlePragmaAndMultiStatementsInternal: Unknown statement.");
		}
		}
	}
	statements = std::move(new_statements);
}

void PragmaHandler::HandlePragmaAndMultiStatements(ClientContextLock &lock,
                                                   vector<unique_ptr<SQLStatement>> &statements) {
	// first check if there are any pragma statements
	bool found_pragma = false;
	for (idx_t i = 0; i < statements.size(); i++) {
		if (statements[i]->type == StatementType::PRAGMA_STATEMENT ||
		    statements[i]->type == StatementType::MULTI_STATEMENT) {
			found_pragma = true;
			break;
		}
	}
	if (!found_pragma) {
		// no pragmas: skip this step
		return;
	}
	context.RunFunctionInTransactionInternal(lock, [&]() {
		HandlePragmaAndMultiStatementsInternal(statements, context.transaction.HasActiveTransaction());
	});
}

bool PragmaHandler::HandlePragma(SQLStatement &statement, string &resulting_query) {
	auto info = statement.Cast<PragmaStatement>().info->Copy();
	QueryErrorContext error_context(statement.stmt_location);
	auto binder = Binder::CreateBinder(context);
	auto bound_info = binder->BindPragma(*info, error_context);
	if (bound_info->function.query) {
		FunctionParameters parameters {bound_info->parameters, bound_info->named_parameters};
		resulting_query = bound_info->function.query(context, parameters);
		return true;
	}
	return false;
}

} // namespace duckdb
