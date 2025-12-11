#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/parser/constraints/unique_constraint.hpp"
#include "duckdb/parser/parsed_data/comment_on_column_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/transaction_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/constraints/bound_unique_constraint.hpp"
#include "duckdb/planner/expression_binder/index_binder.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_simple.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> DuckCatalog::BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
                                                           unique_ptr<LogicalOperator> plan,
                                                           unique_ptr<CreateIndexInfo> create_info,
                                                           unique_ptr<AlterTableInfo> alter_info) {
	D_ASSERT(plan->type == LogicalOperatorType::LOGICAL_GET);
	IndexBinder index_binder(binder, binder.context);
	return index_binder.BindCreateIndex(binder.context, std::move(create_info), table_entry, std::move(plan),
	                                    std::move(alter_info));
}

BoundStatement Binder::BindAlterAddIndex(BoundStatement &result, CatalogEntry &entry,
                                         unique_ptr<AlterInfo> alter_info) {
	auto &table_info = alter_info->Cast<AlterTableInfo>();
	auto &constraint_info = table_info.Cast<AddConstraintInfo>();
	auto &table = entry.Cast<TableCatalogEntry>();
	auto &column_list = table.GetColumns();

	auto bound_constraint = BindUniqueConstraint(*constraint_info.constraint, table_info.name, column_list);
	auto &bound_unique = bound_constraint->Cast<BoundUniqueConstraint>();

	// Create the CreateIndexInfo.
	auto create_index_info = make_uniq<CreateIndexInfo>();
	create_index_info->table = table_info.name;
	create_index_info->index_type = ART::TYPE_NAME;
	create_index_info->constraint_type = IndexConstraintType::PRIMARY;

	for (const auto &physical_index : bound_unique.keys) {
		auto &col = column_list.GetColumn(physical_index);
		unique_ptr<ParsedExpression> parsed = make_uniq<ColumnRefExpression>(col.GetName(), table_info.name);
		create_index_info->expressions.push_back(parsed->Copy());
		create_index_info->parsed_expressions.push_back(parsed->Copy());
	}

	auto unique_constraint = constraint_info.constraint->Cast<UniqueConstraint>();
	auto index_name = unique_constraint.GetName(table_info.name);
	create_index_info->index_name = index_name;
	D_ASSERT(!create_index_info->index_name.empty());

	// Plan the table scan.
	TableDescription table_description(table_info.catalog, table_info.schema, table_info.name);
	auto table_ref = make_uniq<BaseTableRef>(table_description);
	auto bound_table = Bind(*table_ref);
	if (bound_table.plan->type != LogicalOperatorType::LOGICAL_GET) {
		throw BinderException("can only add an index to a base table");
	}
	auto &get = bound_table.plan->Cast<LogicalGet>();
	get.names = column_list.GetColumnNames();

	auto alter_table_info = unique_ptr_cast<AlterInfo, AlterTableInfo>(std::move(alter_info));
	result.plan = table.catalog.BindAlterAddIndex(*this, table, std::move(bound_table.plan),
	                                              std::move(create_index_info), std::move(alter_table_info));
	return std::move(result);
}

BoundStatement Binder::Bind(AlterStatement &stmt) {
	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};

	// Special handling for ALTER DATABASE - doesn't use schema binding
	if (stmt.info->type == AlterType::ALTER_DATABASE) {
		auto &properties = GetStatementProperties();
		properties.return_type = StatementReturnType::NOTHING;
		properties.RegisterDBModify(Catalog::GetSystemCatalog(context), context, DatabaseModificationType::ALTER_TABLE);
		result.plan = make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_ALTER, std::move(stmt.info));
		return result;
	}

	BindSchemaOrCatalog(stmt.info->catalog, stmt.info->schema);

	optional_ptr<CatalogEntry> entry;
	if (stmt.info->type == AlterType::SET_COLUMN_COMMENT) {
		// Extra step for column comments: They can alter a table or a view, and we resolve that here.
		auto &info = stmt.info->Cast<SetColumnCommentInfo>();
		entry = info.TryResolveCatalogEntry(entry_retriever);

	} else {
		// For any other ALTER, we retrieve the catalog entry directly.
		EntryLookupInfo lookup_info(stmt.info->GetCatalogType(), stmt.info->name);
		entry = entry_retriever.GetEntry(stmt.info->catalog, stmt.info->schema, lookup_info, stmt.info->if_not_found);
	}

	auto &properties = GetStatementProperties();
	properties.return_type = StatementReturnType::NOTHING;
	if (!entry) {
		result.plan = make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_ALTER, std::move(stmt.info));
		return result;
	}

	D_ASSERT(!entry->deleted);
	auto &catalog = entry->ParentCatalog();
	if (catalog.IsSystemCatalog()) {
		throw BinderException("Can not comment on System Catalog entries");
	}
	if (!entry->temporary) {
		// We can only alter temporary tables and views in read-only mode.
		properties.RegisterDBModify(catalog, context, DatabaseModificationType::ALTER_TABLE);
	}
	stmt.info->catalog = catalog.GetName();
	stmt.info->schema = entry->ParentSchema().name;

	if (!stmt.info->IsAddPrimaryKey()) {
		/* TODO:
		 *		✅for an alter table - add column:
		 *		✅bind the default expression
		 *		✅check if it's inconsistent/volatile
		 *		if so, create the different plans:
		 *		-Volatile:
		 *			- ALTER TABLE t ADD COLUMN u <type> DEFAULT NULL;
		 *			- UPDATE t SET u = <expression>;
		 *			- ALTER TABLE t ALTER u SET DEFAULT <expression>;
		 *		-Inconsistent:
		 *			- ALTER TABLE t ADD COLUMN u <type> DEFAULT <constant> (by evaluating the expression)
		 *			- ALTER TABLE t ALTER u SET DEFAULT <expression>;
		 */

		// 📜 What default_expression types mean:
		//! CONSISTENT              -> this function always returns the same result when given the same input, no variance
		//! CONSISTENT_WITHIN_QUERY -> this function returns the same result WITHIN the same query/transaction
		//!                            but the result might change across queries (e.g. NOW(), CURRENT_TIME)
		//! VOLATILE                -> the result of this function might change per row (e.g. RANDOM())

		auto &alter_table_info = stmt.info->Cast<AlterTableInfo>();
		if (alter_table_info.alter_table_type == AlterTableType::ADD_COLUMN) {
			auto &add_column_info = alter_table_info.Cast<AddColumnInfo>();
			if (add_column_info.new_column.HasDefaultValue()) {
				unique_ptr<ParsedExpression> default_value = add_column_info.new_column.DefaultValue().Copy();
				// We have a default value, bind it
				ExpressionBinder expr_binder(*this, context);
				try {
					auto bound_default = expr_binder.Bind(default_value);

					vector<unique_ptr<LogicalOperator>> nodes;

					// ALTER TABLE t ADD COLUMN u <type> DEFAULT <constant> (by evaluating the expression)
					// TODO: This is working but I don't know why. How are the wal replays matching the previous values? I don't get it.
					nodes.push_back(std::move(make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_ALTER, std::move(add_column_info.Copy()))));

					// ALTER TABLE t ALTER u SET DEFAULT <expression>;
					// SetDefaultInfo inherits from AlterInfo, so we can use it to make a LogicalSimple and push it do the nodes of our plan.
					AlterEntryData alter_entry_data = stmt.info->GetAlterEntryData();
					auto col_name = add_column_info.new_column.GetName();
					unique_ptr<SetDefaultInfo> alter_info_set_default_expression = make_uniq<SetDefaultInfo>(alter_entry_data, col_name, add_column_info.new_column.DefaultValue().Copy());
					nodes.push_back(std::move(make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_ALTER, std::move(alter_info_set_default_expression))));


					// // Populate nodes
					// if (bound_default->IsVolatile()) {
					// 	// ALTER TABLE t ADD COLUMN u <type> DEFAULT NULL;
					// 	// UPDATE t SET u = <expression>;
					// 	// ALTER TABLE t ALTER u SET DEFAULT <expression>;
					//
					//
					// 	// // FIXME: this code below is wrong, but its to be used as reference
					// 	// auto select_node = make_uniq<SelectNode>();
					// 	// auto &select_list = select_node->select_list;
					// 	// for (auto &col : table.GetColumns().Physical()) {
					// 	// 	select_list.push_back(make_uniq<ColumnRefExpression>(col.Name(), table.name));
					// 	// }
					// 	// select_node->from_table = std::move(from_tbl);
					// 	//
					// 	// auto select_stmt = make_uniq<SelectStatement>();
					// 	// select_stmt->node = std::move(select_node);
					// 	//
					// 	// insert_stmt.select_statement = std::move(select_stmt);
					// 	// auto bound_insert = Bind(insert_stmt);
					// 	// auto insert_plan = std::move(bound_insert.plan);
					//
					// } else if (!bound_default->IsConsistent()) {
					// 	// Value constant_value;
					// 	// auto eval_success = ExpressionExecutor::TryEvaluateScalar(context, *bound_default, constant_value);
					// 	// // Insert the default Value.
					// 	// if (eval_success) {
					// 	// 	printf(constant_value.ToString().c_str());
					// 	// }
					// 	// (void)bound_default;
					//
					// 	// ALTER TABLE t ADD COLUMN u <type> DEFAULT <constant> (by evaluating the expression)
					// 	// TODO: This is working but I don't know why. How are the wal replays matching the previous values? I don't get it.
					// 	nodes.push_back(std::move(make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_ALTER, std::move(add_column_info.Copy()))));
					//
					// 	// ALTER TABLE t ALTER u SET DEFAULT <expression>;
					// 	// SetDefaultInfo inherits from AlterInfo, so we can use it to make a LogicalSimple and push it do the nodes of our plan.
					// 	AlterEntryData alter_entry_data = stmt.info->GetAlterEntryData();
					// 	auto col_name = add_column_info.new_column.GetName();
					// 	unique_ptr<SetDefaultInfo> alter_info_set_default_expression = make_uniq<SetDefaultInfo>(alter_entry_data, col_name, add_column_info.new_column.DefaultValue().Copy()); //TODO: Q: We're making another copy here. Is that ok?
					// 	nodes.push_back(std::move(make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_ALTER, std::move(alter_info_set_default_expression))));
					//
					//
					// }
					result.plan =  UnionOperators(std::move(nodes));
					return result;


				} catch (const BinderException &e) {
					throw e; // rethrow the exception
				}
			}
		}

		result.plan = make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_ALTER, std::move(stmt.info));
		return result;
	}

	return BindAlterAddIndex(result, *entry, std::move(stmt.info));
}

BoundStatement Binder::Bind(TransactionStatement &stmt) {
	auto &properties = GetStatementProperties();

	// Transaction statements do not require a valid transaction.
	properties.requires_valid_transaction = stmt.info->type == TransactionType::BEGIN_TRANSACTION;

	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};
	result.plan = make_uniq<LogicalSimple>(LogicalOperatorType::LOGICAL_TRANSACTION, std::move(stmt.info));
	properties.return_type = StatementReturnType::NOTHING;
	return result;
}

} // namespace duckdb
