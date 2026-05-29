#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <limits>
#include <map>
#include "postgres_parser.hpp"
#include "b_plus_tree.h"
#include "table.h"
#include "catalog.h"
#include "page_manager.h"
#include "definitions.h" // Include common definitions
#include "nodes/parsenodes.hpp"
#include <cstring> // For strcmp
#include "nodes/pg_list.hpp"
#include "log_manager.h"
#include "lock_manager.h"
#include "mvcc.h"
#include "query_utils.h"
#include "operators/query_executor.h"
#include <optional>

// EvaluateWhereClause is now in query_utils.cpp

// Function to execute a SQL query - returns false if should exit
bool ExecuteQuery(const std::string& query, duckdb::PostgresParser& parser, 
                 toydb::Catalog& catalog, toydb::PageManager& page_manager,
                 toydb::LogManager& log_manager, toydb::LockManager& lock_manager,
                 toydb::MVCCManager& mvcc_manager,
                 std::optional<int>& session_tx_id,
                 std::optional<toydb::TxId>& session_snapshot);

// Helper to print std::vector<std::string>
// Moved to table.h
// std::string FormatRow(const std::vector<std::string>& row) {
//     std::string formatted_row = "[";
//     for (size_t i = 0; i < row.size(); ++i) {
//         formatted_row += "'" + row[i] + "'";
//         if (i < row.size() - 1) {
//             formatted_row += ", ";
//         }
//     }
//     formatted_row += "]";
//     return formatted_row;
// }

// ExecuteQuery function implementation
bool ExecuteQuery(const std::string& query, duckdb::PostgresParser& parser, 
                 toydb::Catalog& catalog, toydb::PageManager& page_manager,
                 toydb::LogManager& log_manager, toydb::LockManager& lock_manager,
                 toydb::MVCCManager& mvcc_manager,
                 std::optional<int>& session_tx_id,
                 std::optional<toydb::TxId>& session_snapshot) {
    std::string trimmed_query = toydb::Trim(query);
    
    if (trimmed_query.empty()) {
        return true; // Empty query, continue
    }
    
    // Check for exit commands (remove trailing semicolon if present)
    std::string query_for_check = trimmed_query;
    if (!query_for_check.empty() && query_for_check.back() == ';') {
        query_for_check = query_for_check.substr(0, query_for_check.size() - 1);
        query_for_check = toydb::Trim(query_for_check);
    }
    std::string upper_query = query_for_check;
    std::transform(upper_query.begin(), upper_query.end(), upper_query.begin(), ::toupper);
    if (upper_query == "QUIT" || upper_query == "EXIT" || upper_query == ".EXIT") {
        return false; // Signal to exit
    }
    
    parser.Parse(trimmed_query);
    
    if (!parser.success) {
        std::cerr << "Error: " << parser.error_message << std::endl;
        return true;
    }
    
    duckdb_libpgquery::PGList* parse_tree = parser.parse_tree;
    if (parse_tree == nullptr || parse_tree->head == nullptr) {
        std::cerr << "Error: Empty parse tree." << std::endl;
        return true;
    }
    
    duckdb_libpgquery::PGNode* stmt_node = (duckdb_libpgquery::PGNode*)parse_tree->head->data.ptr_value;
    if (stmt_node->type != duckdb_libpgquery::T_PGRawStmt) {
        std::cerr << "Error: Invalid statement type." << std::endl;
        return true;
    }
    
    duckdb_libpgquery::PGRawStmt* raw_stmt = (duckdb_libpgquery::PGRawStmt*)stmt_node;
    
    // Handle CREATE TABLE
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGCreateStmt) {
        duckdb_libpgquery::PGCreateStmt* create_stmt = (duckdb_libpgquery::PGCreateStmt*)raw_stmt->stmt;
        std::string table_name = create_stmt->relation->relname;
        std::vector<toydb::Column> columns;

        for (auto* col_node = create_stmt->tableElts->head; col_node != nullptr; col_node = col_node->next) {
            duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)col_node->data.ptr_value;
            if (node_data->type == duckdb_libpgquery::T_PGColumnDef) {
                duckdb_libpgquery::PGColumnDef* col_def = (duckdb_libpgquery::PGColumnDef*)node_data;
                std::string col_name = col_def->colname;
                std::string type_name = toydb::PgValueToString((duckdb_libpgquery::PGValue*)col_def->typeName->names->tail->data.ptr_value);

                toydb::ColumnType col_type;
                size_t col_length = 0;

                if (type_name == "int4") {
                    col_type = toydb::ColumnType::INT;
                } else if (type_name == "varchar") {
                    col_type = toydb::ColumnType::VARCHAR;
                    if (col_def->typeName->typmods != nullptr && col_def->typeName->typmods->head != nullptr) {
                        duckdb_libpgquery::PGNode* typmod_node = (duckdb_libpgquery::PGNode*)col_def->typeName->typmods->head->data.ptr_value;
                        if (typmod_node->type == duckdb_libpgquery::T_PGAConst) {
                            duckdb_libpgquery::PGAConst* const_node = (duckdb_libpgquery::PGAConst*)typmod_node;
                            if (const_node->val.type == duckdb_libpgquery::T_PGInteger) {
                                col_length = const_node->val.val.ival;
                            }
                        }
                    }
                } else {
                    std::cerr << "Unsupported column type: " << type_name << std::endl;
                    continue;
                }
                columns.emplace_back(col_name, col_type, col_length);
            }
        }
        auto new_table = std::make_unique<toydb::Table>(table_name, std::move(columns), page_manager);
        catalog.AddTable(std::move(new_table));
        
        // Immediately save catalog after creating table
        toydb::Page* catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
        if (catalog_page == nullptr) {
            uint32_t new_catalog_page_id = page_manager.NewPage();
            if (new_catalog_page_id != toydb::CATALOG_PAGE_ID) {
                std::cerr << "Error: Catalog page ID mismatch!" << std::endl;
                return true;
            }
            catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
        }
        catalog.Serialize(catalog_page);
        page_manager.FlushPage(toydb::CATALOG_PAGE_ID);
        
        std::cout << "Table '" << table_name << "' created successfully." << std::endl;
        return true;
    }
    // Handle DROP TABLE
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGDropStmt) {
        auto* drop_stmt = (duckdb_libpgquery::PGDropStmt*)raw_stmt->stmt;
        if (drop_stmt->removeType != duckdb_libpgquery::PG_OBJECT_TABLE) {
            std::cerr << "Error: only DROP TABLE is supported." << std::endl;
            return true;
        }

        std::string table_name;
        if (drop_stmt->objects && drop_stmt->objects->head) {
            auto* obj_list = (duckdb_libpgquery::PGList*)drop_stmt->objects->head->data.ptr_value;
            if (obj_list && obj_list->head) {
                table_name = toydb::PgValueToString((duckdb_libpgquery::PGValue*)obj_list->head->data.ptr_value);
            }
        }

        if (table_name.empty()) {
            std::cerr << "Error: DROP TABLE missing table name." << std::endl;
            return true;
        }

        if (!catalog.GetTable(table_name)) {
            std::cerr << "Error: Table '" << table_name << "' not found." << std::endl;
            return true;
        }

        catalog.RemoveTable(table_name);
        toydb::Page* catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
        if (catalog_page == nullptr) {
            uint32_t new_catalog_page_id = page_manager.NewPage();
            if (new_catalog_page_id != toydb::CATALOG_PAGE_ID) {
                std::cerr << "Error: Catalog page ID mismatch!" << std::endl;
                return true;
            }
            catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
        }
        catalog.Serialize(catalog_page);
        page_manager.FlushPage(toydb::CATALOG_PAGE_ID);

        std::cout << "Table '" << table_name << "' dropped." << std::endl;
        return true;
    }
    
    // Handle transaction control (BEGIN/COMMIT/ROLLBACK)
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGTransactionStmt) {
        duckdb_libpgquery::PGTransactionStmt* tx_stmt = (duckdb_libpgquery::PGTransactionStmt*)raw_stmt->stmt;
        using Kind = duckdb_libpgquery::PGTransactionStmtKind;
        if (tx_stmt->kind == Kind::PG_TRANS_STMT_BEGIN || tx_stmt->kind == Kind::PG_TRANS_STMT_START) {
            if (session_tx_id.has_value()) {
                std::cerr << "Error: transaction already active (TX " << session_tx_id.value() << ")." << std::endl;
            } else {
                session_tx_id = log_manager.BeginTransaction();
                // 创建快照：记录当前已提交的事务状态
                session_snapshot = mvcc_manager.BeginSnapshot(session_tx_id.value());
            }
        } else if (tx_stmt->kind == Kind::PG_TRANS_STMT_COMMIT) {
            if (!session_tx_id.has_value()) {
                std::cerr << "Error: COMMIT with no active transaction." << std::endl;
            } else {
                int tx_id = session_tx_id.value();
                
                // 在CommitTransaction之前，先将该事务创建的版本标记为committed
                // 这样CommitTransaction刷新页面时就能包含更新后的版本链
                for (const auto& pair : catalog.GetTables()) {
                    toydb::Table* table = pair.second.get();
                    // 获取所有键值对
                    auto all_values = table->bptree_->GetAllValues();
                    bool updated = false;
                    for (auto& kv : all_values) {
                        // 注意：GetAllValues返回的是值拷贝，需要重新获取版本链
                        std::vector<toydb::VersionedRow> version_chain;
                        if (table->bptree_->Search(kv.first, version_chain)) {
                            bool chain_updated = false;
                            for (auto& ver : version_chain) {
                                if (ver.create_tx == tx_id && !ver.committed) {
                                    ver.committed = true;
                                    chain_updated = true;
                                }
                            }
                            if (chain_updated) {
                                // 更新版本链
                                table->bptree_->Delete(kv.first);
                                table->bptree_->Insert(kv.first, version_chain);
                                updated = true;
                            }
                        }
                    }
                    if (updated) {
                        table->SetRootPageId(table->bptree_->GetRootPageId());
                    }
                }
                
                // 现在调用CommitTransaction，它会刷新所有页面（包括更新后的版本链）
                if (log_manager.CommitTransaction(tx_id, page_manager)) {
                    // 通知 MVCCManager 事务已提交
                    mvcc_manager.OnTransactionCommitted(tx_id);
                    
                    // COMMIT时保存catalog，确保所有表的root_page_id都持久化
                    toydb::Page* catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
                    if (catalog_page != nullptr) {
                        catalog.Serialize(catalog_page);
                        page_manager.FlushPage(toydb::CATALOG_PAGE_ID);
                    }
                    
                    lock_manager.ReleaseLocks(tx_id);
                    session_tx_id.reset();
                    session_snapshot.reset();
                }
            }
        } else if (tx_stmt->kind == Kind::PG_TRANS_STMT_ROLLBACK) {
            if (!session_tx_id.has_value()) {
                std::cerr << "Error: ROLLBACK with no active transaction." << std::endl;
            } else {
                int tx_id = session_tx_id.value();
                if (log_manager.RollbackTransaction(tx_id, catalog, page_manager)) {
                    // 通知 MVCCManager 事务已回滚
                    mvcc_manager.OnTransactionRolledBack(tx_id);
                    lock_manager.ReleaseLocks(tx_id);
                    session_tx_id.reset();
                    session_snapshot.reset();
                }
            }
        } else {
            std::cerr << "Error: Unsupported transaction command." << std::endl;
        }
        return true;
    }
    
    // Handle INSERT
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGInsertStmt) {
        duckdb_libpgquery::PGInsertStmt* insert_stmt = (duckdb_libpgquery::PGInsertStmt*)raw_stmt->stmt;
        std::string table_name = insert_stmt->relation->relname;
        toydb::Table* target_table = catalog.GetTable(table_name);

        if (target_table) {
            if (insert_stmt->selectStmt && insert_stmt->selectStmt->type == duckdb_libpgquery::T_PGSelectStmt) {
                duckdb_libpgquery::PGSelectStmt* select_stmt = (duckdb_libpgquery::PGSelectStmt*)insert_stmt->selectStmt;
                if (select_stmt->valuesLists && select_stmt->valuesLists->head) {
                    duckdb_libpgquery::PGList* values_list = (duckdb_libpgquery::PGList*)select_stmt->valuesLists->head->data.ptr_value;
                    std::vector<std::string> row_values;
                    for (auto* value_node = values_list->head; value_node != nullptr; value_node = value_node->next) {
                        duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)value_node->data.ptr_value;
                        if (node_data->type == duckdb_libpgquery::T_PGAConst) {
                            duckdb_libpgquery::PGAConst* const_node = (duckdb_libpgquery::PGAConst*)node_data;
                            if (const_node->val.type == duckdb_libpgquery::T_PGInteger) {
                                row_values.push_back(std::to_string(const_node->val.val.ival));
                            } else if (const_node->val.type == duckdb_libpgquery::T_PGString) {
                                row_values.push_back(const_node->val.val.str);
                            }
                        }
                    }

                    if (!row_values.empty()) {
                        int key = std::stoi(row_values[0]); 
                        uint32_t old_root_id = target_table->bptree_->GetRootPageId(); // 使用bptree的root_page_id，而不是Table的
                        int tx_id_for_row = -1;
                        if (session_tx_id.has_value()) {
                            tx_id_for_row = session_tx_id.value();
                            if (!lock_manager.AcquireRowLock(table_name, key, tx_id_for_row, toydb::LockMode::WRITE)) {
                                std::cerr << "Error: could not acquire row write lock for TX " << tx_id_for_row << std::endl;
                                return true;
                            }
                            log_manager.LogInsert(tx_id_for_row, table_name, key, row_values);
                        }
                        toydb::VersionedRow v;
                        v.data = row_values;
                        v.create_tx = tx_id_for_row;
                        v.delete_tx = toydb::INVALID_TX_ID;
                        v.committed = !session_tx_id.has_value(); // 非事务语句插入的立即视为已提交版本
                        // 检查是否已存在版本链，如果存在则追加，否则创建新链
                        std::vector<toydb::VersionedRow> version_chain;
                        std::vector<toydb::VersionedRow> existing_chain;
                        if (target_table->bptree_->Search(key, existing_chain)) {
                            version_chain = existing_chain; // 保留历史版本
                        }
                        version_chain.push_back(v); // 添加新版本
                        target_table->bptree_->Delete(key); // 删除旧链（如果存在）
                        target_table->bptree_->Insert(key, version_chain); // 插入新链
                        uint32_t new_root_id = target_table->bptree_->GetRootPageId();
                        target_table->SetRootPageId(new_root_id);
                        
                        // Flush all dirty pages first to ensure B+ tree pages are saved
                        page_manager.FlushAllPages();
                        
                        // Always save catalog after INSERT to ensure root_page_id is persisted
                        // (root_page_id may change from INVALID to a valid page ID)
                        toydb::Page* catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
                        if (catalog_page != nullptr) {
                            catalog.Serialize(catalog_page);
                            page_manager.FlushPage(toydb::CATALOG_PAGE_ID);
                        }
                        
                        std::cout << "Inserted 1 row." << std::endl;
                    }
                }
            }
        } else {
            std::cerr << "Error: Table '" << table_name << "' not found." << std::endl;
        }
        return true;
    }
    
    // Handle SELECT - 使用火山模型
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGSelectStmt) {
        duckdb_libpgquery::PGSelectStmt* select_stmt = (duckdb_libpgquery::PGSelectStmt*)raw_stmt->stmt;
        
        // 检查表是否存在
        std::string table_name;
        if (select_stmt->fromClause && select_stmt->fromClause->head) {
            duckdb_libpgquery::PGNode* from_node = (duckdb_libpgquery::PGNode*)select_stmt->fromClause->head->data.ptr_value;
            if (from_node->type == duckdb_libpgquery::T_PGRangeVar) {
                duckdb_libpgquery::PGRangeVar* range_var = (duckdb_libpgquery::PGRangeVar*)from_node;
                table_name = range_var->relname;
            } else if (from_node->type == duckdb_libpgquery::T_PGJoinExpr) {
                duckdb_libpgquery::PGJoinExpr* join_expr = (duckdb_libpgquery::PGJoinExpr*)from_node;
                if (join_expr->larg && join_expr->larg->type == duckdb_libpgquery::T_PGRangeVar) {
                    duckdb_libpgquery::PGRangeVar* left_range = (duckdb_libpgquery::PGRangeVar*)join_expr->larg;
                    table_name = left_range->relname;
                }
            }
        }
        
        // 获取表并检查锁（对于单表查询）
        if (!table_name.empty() && session_tx_id.has_value()) {
            toydb::Table* target_table = catalog.GetTable(table_name);
            if (target_table) {
                int current_tx_id = session_tx_id.value();
                // 尝试行级读锁（如果 WHERE 是主键等值查询）
                bool use_row_lock = false;
                int search_key = -1;
                if (select_stmt->whereClause &&
                    select_stmt->whereClause->type == duckdb_libpgquery::T_PGAExpr) {
                    auto* expr = (duckdb_libpgquery::PGAExpr*)select_stmt->whereClause;
                    if (expr->kind == duckdb_libpgquery::PG_AEXPR_OP &&
                        strcmp(toydb::PgValueToString((duckdb_libpgquery::PGValue*)expr->name->head->data.ptr_value).c_str(), "=") == 0) {
                        toydb::WhereValue left_val = toydb::ExtractWhereValue(expr->lexpr, target_table);
                        toydb::WhereValue right_val = toydb::ExtractWhereValue(expr->rexpr, target_table);
                        if ((left_val.is_column && left_val.column_name == target_table->GetColumns()[0].name && !right_val.is_column) ||
                            (!left_val.is_column && right_val.is_column && right_val.column_name == target_table->GetColumns()[0].name)) {
                            use_row_lock = true;
                            search_key = left_val.is_column ? right_val.constant_int : left_val.constant_int;
                        }
                    }
                }
                
                if (use_row_lock) {
                    if (!lock_manager.AcquireRowLock(table_name, search_key, current_tx_id, toydb::LockMode::READ)) {
                        std::cerr << "Error: could not acquire row read lock for TX " << current_tx_id << std::endl;
                        return true;
                    }
                } else {
                    if (!lock_manager.AcquireTableLock(table_name, current_tx_id, toydb::LockMode::READ)) {
                        std::cerr << "Error: could not acquire table read lock for TX " << current_tx_id << std::endl;
                        return true;
                    }
                }
            }
        }
        
        // 使用火山模型执行查询
        toydb::QueryExecutor executor(catalog, mvcc_manager, session_tx_id, session_snapshot);
        auto query_plan = executor.BuildQueryPlan(select_stmt);
        if (query_plan) {
            executor.ExecuteAndPrint(std::move(query_plan));
        }
        // 如果 query_plan 为 nullptr，QueryExecutor 已经输出了错误消息
        
        return true;
    }
    
    // Handle UPDATE
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGUpdateStmt) {
        duckdb_libpgquery::PGUpdateStmt* update_stmt = (duckdb_libpgquery::PGUpdateStmt*)raw_stmt->stmt;
        std::string table_name = update_stmt->relation->relname;
        toydb::Table* target_table = catalog.GetTable(table_name);

        if (target_table) {
            if (session_tx_id.has_value()) {
                int tx_id = session_tx_id.value();
                if (!lock_manager.AcquireTableLock(table_name, tx_id, toydb::LockMode::WRITE)) {
                    std::cerr << "Error: could not acquire table write lock for TX " << tx_id << std::endl;
                    return true;
                }
            }
            int update_key = -1;
            if (update_stmt->whereClause) {
                duckdb_libpgquery::PGAExpr* expr = (duckdb_libpgquery::PGAExpr*)update_stmt->whereClause;
                if (expr->kind == duckdb_libpgquery::PG_AEXPR_OP && strcmp(toydb::PgValueToString((duckdb_libpgquery::PGValue*)expr->name->head->data.ptr_value).c_str(), "=") == 0) {
                    // Try rexpr first (column = value)
                    if (expr->rexpr && expr->rexpr->type == duckdb_libpgquery::T_PGAConst) {
                        duckdb_libpgquery::PGAConst* const_val = (duckdb_libpgquery::PGAConst*)expr->rexpr;
                        if (const_val->val.type == duckdb_libpgquery::T_PGInteger) {
                            update_key = const_val->val.val.ival;
                        }
                    }
                    // Try lexpr if rexpr didn't work (value = column)
                    if (update_key == -1 && expr->lexpr && expr->lexpr->type == duckdb_libpgquery::T_PGAConst) {
                        duckdb_libpgquery::PGAConst* const_val = (duckdb_libpgquery::PGAConst*)expr->lexpr;
                        if (const_val->val.type == duckdb_libpgquery::T_PGInteger) {
                            update_key = const_val->val.val.ival;
                        }
                    }
                }
            }

            if (update_key != -1) {
                std::vector<toydb::VersionedRow> version_chain;
                if (target_table->bptree_->Search(update_key, version_chain)) {
                    // 从版本链中找到可见版本
                    const toydb::VersionedRow* current_visible = toydb::FindVisibleVersion(version_chain, session_tx_id, session_snapshot, mvcc_manager);
                    if (!current_visible) {
                        std::cout << "0 rows updated" << std::endl;
                        return true;
                    }
                    
                    std::vector<std::string> current_row = current_visible->data;
                    const auto& table_columns = target_table->GetColumns();
                    std::vector<std::string> updated_row = current_row;

                    for (auto* target_node = update_stmt->targetList->head; target_node != nullptr; target_node = target_node->next) {
                        duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)target_node->data.ptr_value;
                        if (node_data->type == duckdb_libpgquery::T_PGResTarget) {
                            duckdb_libpgquery::PGResTarget* res_target = (duckdb_libpgquery::PGResTarget*)node_data;
                            std::string col_name = res_target->name ? res_target->name : "";
                            
                            size_t col_index = SIZE_MAX;
                            for (size_t i = 0; i < table_columns.size(); ++i) {
                                if (table_columns[i].name == col_name) {
                                    col_index = i;
                                    break;
                                }
                            }

                            if (col_index != SIZE_MAX && res_target->val) {
                                std::string new_value;
                                // 1) 直接常量赋值：SET col = 123 / 'abc'
                                if (res_target->val->type == duckdb_libpgquery::T_PGAConst) {
                                    duckdb_libpgquery::PGAConst* const_node = (duckdb_libpgquery::PGAConst*)res_target->val;
                                    if (const_node->val.type == duckdb_libpgquery::T_PGInteger) {
                                        new_value = std::to_string(const_node->val.val.ival);
                                    } else if (const_node->val.type == duckdb_libpgquery::T_PGString) {
                                        new_value = const_node->val.val.str;
                                    }
                                }
                                // 2) 支持简单算术：SET col = col +/- const_int
                                else if (res_target->val->type == duckdb_libpgquery::T_PGAExpr) {
                                    duckdb_libpgquery::PGAExpr* expr = (duckdb_libpgquery::PGAExpr*)res_target->val;
                                    if (expr->kind == duckdb_libpgquery::PG_AEXPR_OP && expr->name && expr->name->head) {
                                        std::string op = toydb::PgValueToString((duckdb_libpgquery::PGValue*)expr->name->head->data.ptr_value);
                                        // 仅支持一侧是本列，另一侧是整数常量
                                        int base_val = 0;
                                        bool base_ok = false;
                                        int delta = 0;
                                        bool delta_ok = false;

                                        // 左侧是否是列
                                        if (expr->lexpr && expr->lexpr->type == duckdb_libpgquery::T_PGColumnRef) {
                                            auto* col_ref = (duckdb_libpgquery::PGColumnRef*)expr->lexpr;
                                            if (col_ref->fields && col_ref->fields->head) {
                                                std::string ref_name = toydb::PgValueToString((duckdb_libpgquery::PGValue*)col_ref->fields->head->data.ptr_value);
                                                if (ref_name == col_name && col_index < current_row.size()) {
                                                    try {
                                                        base_val = std::stoi(current_row[col_index]);
                                                        base_ok = true;
                                                    } catch (...) {}
                                                }
                                            }
                                        }
                                        // 右侧是否是整数常量
                                        if (expr->rexpr && expr->rexpr->type == duckdb_libpgquery::T_PGAConst) {
                                            auto* const_node = (duckdb_libpgquery::PGAConst*)expr->rexpr;
                                            if (const_node->val.type == duckdb_libpgquery::T_PGInteger) {
                                                delta = const_node->val.val.ival;
                                                delta_ok = true;
                                            }
                                        }
                                        // 也支持 const +/- col
                                        if (!base_ok && expr->rexpr && expr->rexpr->type == duckdb_libpgquery::T_PGColumnRef) {
                                            auto* col_ref = (duckdb_libpgquery::PGColumnRef*)expr->rexpr;
                                            if (col_ref->fields && col_ref->fields->head) {
                                                std::string ref_name = toydb::PgValueToString((duckdb_libpgquery::PGValue*)col_ref->fields->head->data.ptr_value);
                                                if (ref_name == col_name && col_index < current_row.size()) {
                                                    try {
                                                        base_val = std::stoi(current_row[col_index]);
                                                        base_ok = true;
                                                    } catch (...) {}
                                                }
                                            }
                                        }
                                        if (!delta_ok && expr->lexpr && expr->lexpr->type == duckdb_libpgquery::T_PGAConst) {
                                            auto* const_node = (duckdb_libpgquery::PGAConst*)expr->lexpr;
                                            if (const_node->val.type == duckdb_libpgquery::T_PGInteger) {
                                                delta = const_node->val.val.ival;
                                                delta_ok = true;
                                            }
                                        }

                                        if (base_ok && delta_ok) {
                                            if (op == "+") {
                                                new_value = std::to_string(base_val + delta);
                                            } else if (op == "-") {
                                                // 如果写成 const - col，严格来说不支持，这里只处理 col - const
                                                new_value = std::to_string(base_val - delta);
                                            }
                                        }
                                    }
                                }

                                if (!new_value.empty() && col_index < updated_row.size()) {
                                    updated_row[col_index] = new_value;
                                }
                            }
                        }
                    }

                    if (session_tx_id.has_value()) {
                        int tx_id = session_tx_id.value();
                        if (!lock_manager.AcquireRowLock(table_name, update_key, tx_id, toydb::LockMode::WRITE)) {
                            std::cerr << "Error: could not acquire row write lock for TX " << tx_id << std::endl;
                            return true;
                        }
                        log_manager.LogUpdate(tx_id, table_name, update_key, current_row, updated_row);
                        // MVCC UPDATE：将新版本添加到版本链
                        toydb::VersionedRow new_ver;
                        new_ver.data = updated_row;
                        new_ver.create_tx = tx_id;
                        new_ver.delete_tx = toydb::INVALID_TX_ID;
                        new_ver.committed = false;
                        version_chain.push_back(new_ver); // 添加到版本链
                    target_table->bptree_->Delete(update_key);
                        target_table->bptree_->Insert(update_key, version_chain);
                    } else {
                        // 非事务 UPDATE：创建已提交的新版本
                        toydb::VersionedRow new_ver;
                        new_ver.data = updated_row;
                        new_ver.create_tx = toydb::INVALID_TX_ID;
                        new_ver.delete_tx = toydb::INVALID_TX_ID;
                        new_ver.committed = true;
                        version_chain.push_back(new_ver); // 添加到版本链
                        target_table->bptree_->Delete(update_key);
                        target_table->bptree_->Insert(update_key, version_chain);
                    }
                    target_table->SetRootPageId(target_table->bptree_->GetRootPageId());
                    
                    // 如果在事务中，不立即保存catalog（等COMMIT时保存）
                    // 如果不在事务中，立即保存catalog
                    if (!session_tx_id.has_value()) {
                        // 非事务UPDATE：立即保存catalog和刷新页面
                        page_manager.FlushAllPages();
                        toydb::Page* catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
                        if (catalog_page != nullptr) {
                            catalog.Serialize(catalog_page);
                            page_manager.FlushPage(toydb::CATALOG_PAGE_ID);
                        }
                    }
                    // 事务中的UPDATE：等COMMIT时统一刷新和保存
                    
                    std::cout << "Updated 1 row." << std::endl;
                } else {
                    std::cout << "0 rows updated" << std::endl;
                }
            }
        } else {
            std::cerr << "Error: Table '" << table_name << "' not found." << std::endl;
        }
        return true;
    }
    
    // Handle DELETE
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGDeleteStmt) {
        duckdb_libpgquery::PGDeleteStmt* delete_stmt = (duckdb_libpgquery::PGDeleteStmt*)raw_stmt->stmt;
        std::string table_name = delete_stmt->relation->relname;
        toydb::Table* target_table = catalog.GetTable(table_name);

        if (target_table) {
            // 如果在事务中，先获取表级写锁，后续逐行处理
            if (session_tx_id.has_value()) {
                int tx_id = session_tx_id.value();
                if (!lock_manager.AcquireTableLock(table_name, tx_id, toydb::LockMode::WRITE)) {
                    std::cerr << "Error: could not acquire table write lock for TX " << tx_id << std::endl;
                    return true;
                }
            }

            bool has_where = delete_stmt->whereClause != nullptr;
            int delete_key = -1;

            // 支持 DELETE … WHERE pk = const
            if (has_where) {
                duckdb_libpgquery::PGAExpr* expr = (duckdb_libpgquery::PGAExpr*)delete_stmt->whereClause;
                if (expr->kind == duckdb_libpgquery::PG_AEXPR_OP && strcmp(toydb::PgValueToString((duckdb_libpgquery::PGValue*)expr->name->head->data.ptr_value).c_str(), "=") == 0) {
                    // Try rexpr first (column = value)
                    if (expr->rexpr && expr->rexpr->type == duckdb_libpgquery::T_PGAConst) {
                        duckdb_libpgquery::PGAConst* const_val = (duckdb_libpgquery::PGAConst*)expr->rexpr;
                        if (const_val->val.type == duckdb_libpgquery::T_PGInteger) {
                            delete_key = const_val->val.val.ival;
                        }
                    }
                    // Try lexpr if rexpr didn't work (value = column)
                    if (delete_key == -1 && expr->lexpr && expr->lexpr->type == duckdb_libpgquery::T_PGAConst) {
                        duckdb_libpgquery::PGAConst* const_val = (duckdb_libpgquery::PGAConst*)expr->lexpr;
                        if (const_val->val.type == duckdb_libpgquery::T_PGInteger) {
                            delete_key = const_val->val.val.ival;
                        }
                    }
                }
            }

            size_t deleted_count = 0;

            if (has_where && delete_key != -1) {
                // 单键删除
                std::vector<toydb::VersionedRow> version_chain;
                bool found = target_table->bptree_->Search(delete_key, version_chain);
                if (found) {
                    const toydb::VersionedRow* visible = toydb::FindVisibleVersion(version_chain, session_tx_id, session_snapshot, mvcc_manager);
                    if (visible) {
                        std::vector<std::string> old_row = visible->data;
                        if (session_tx_id.has_value()) {
                            int tx_id = session_tx_id.value();
                            log_manager.LogDelete(tx_id, table_name, delete_key, old_row);
                            for (auto& ver : version_chain) {
                                if (&ver == visible) {
                                    ver.delete_tx = tx_id;
                                    ver.committed = false;
                                    break;
                                }
                            }
                            target_table->bptree_->Delete(delete_key);
                            target_table->bptree_->Insert(delete_key, version_chain);
                        } else {
                            // 非事务：直接物理删除
                            target_table->bptree_->Delete(delete_key);
                        }
                        deleted_count = 1;
                    }
                }
            } else if (!has_where) {
                // DELETE 全表
                auto all_values = target_table->bptree_->GetAllValues();
                for (const auto& kv : all_values) {
                    int key = kv.first;
                    std::vector<toydb::VersionedRow> version_chain = kv.second;
                    const toydb::VersionedRow* visible = toydb::FindVisibleVersion(version_chain, session_tx_id, session_snapshot, mvcc_manager);
                    if (!visible) {
                        continue;
                    }

                    if (session_tx_id.has_value()) {
                        int tx_id = session_tx_id.value();
                        log_manager.LogDelete(tx_id, table_name, key, visible->data);
                        for (auto& ver : version_chain) {
                            if (&ver == visible) {
                                ver.delete_tx = tx_id;
                                ver.committed = false;
                                break;
                            }
                        }
                        target_table->bptree_->Delete(key);
                        target_table->bptree_->Insert(key, version_chain);
                    } else {
                        target_table->bptree_->Delete(key);
                    }
                    ++deleted_count;
                }
            }

            target_table->SetRootPageId(target_table->bptree_->GetRootPageId());

            if (deleted_count == 0) {
                std::cout << "0 rows deleted" << std::endl;
            } else if (deleted_count == 1) {
                std::cout << "Deleted 1 row." << std::endl;
            } else {
                std::cout << "Deleted " << deleted_count << " rows." << std::endl;
            }
        } else {
            std::cerr << "Error: Table '" << table_name << "' not found." << std::endl;
        }
        return true;
    }
    
    // Handle SHOW TABLES
    if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGVariableShowStmt) {
        duckdb_libpgquery::PGVariableShowStmt* show_stmt = (duckdb_libpgquery::PGVariableShowStmt*)raw_stmt->stmt;
        if (show_stmt->name) {
            std::string show_name = show_stmt->name;
            std::transform(show_name.begin(), show_name.end(), show_name.begin(), ::tolower);
            // Remove quotes if present
            if (show_name.size() >= 2 && show_name[0] == '"' && show_name.back() == '"') {
                show_name = show_name.substr(1, show_name.size() - 2);
            }
            if (show_name == "tables") {
                for (const auto& pair : catalog.GetTables()) {
                    std::cout << pair.first << std::endl;
                }
            }
        }
        return true;
    }
    
    std::cerr << "Error: Unsupported statement type." << std::endl;
    return true;
}

int main() {
    std::cout << "ToyDB - A Simple Database System" << std::endl;
    std::cout << "Type 'QUIT' or 'EXIT' to exit." << std::endl;
    std::cout << "toydb> ";

    duckdb::PostgresParser parser;
    toydb::PageManager page_manager("toydb.db");
    toydb::Catalog catalog;
    toydb::LogManager log_manager("toydb.log");
    toydb::LockManager lock_manager;
    toydb::MVCCManager mvcc_manager;

    // Load catalog from disk on startup
    toydb::Page* catalog_page_raw = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
    if (catalog_page_raw != nullptr) {
        catalog.Deserialize(catalog_page_raw, page_manager);
        std::cout << "Database loaded." << std::endl;
    }

    // Interactive loop
    std::string line;
    std::string query;
    std::optional<int> session_tx_id;
    std::optional<toydb::TxId> session_snapshot; // 快照隔离：事务开始时的快照
    
    while (true) {
        if (std::cin.eof() || !std::cin.good()) {
            break; // EOF or error
        }
        
        std::cout << "toydb> ";
        std::cout.flush();
        
        if (!std::getline(std::cin, line)) {
            break; // EOF or error
        }
        
        query += line;
        
        // Check for exit commands (even without semicolon)
        std::string trimmed_line = toydb::Trim(line);
        std::string upper_line = trimmed_line;
        std::transform(upper_line.begin(), upper_line.end(), upper_line.begin(), ::toupper);
        if (upper_line == "QUIT" || upper_line == "EXIT" || upper_line == ".EXIT") {
            // Exit immediately, even without semicolon
            break;
        }
        
        // Check if line ends with semicolon (end of query)
        if (!line.empty() && line.back() == ';') {
            query = toydb::Trim(query);
            if (!query.empty() && query != ";") {
                if (!ExecuteQuery(query, parser, catalog, page_manager, log_manager, lock_manager, mvcc_manager, session_tx_id, session_snapshot)) {
                    break; // Exit command
                }
            }
            query.clear();
        } else if (!line.empty()) {
            // Continue multiline query
            query += " ";
        }
    }

    // Save catalog to disk on shutdown
    toydb::Page* catalog_write_page_raw = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
    if (catalog_write_page_raw == nullptr) {
        uint32_t new_catalog_page_id = page_manager.NewPage();
        if (new_catalog_page_id != toydb::CATALOG_PAGE_ID) {
            std::cerr << "Error: Catalog page ID mismatch!" << std::endl;
            return 1;
        }
        catalog_write_page_raw = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
    }
    catalog.Serialize(catalog_write_page_raw);
    page_manager.FlushPage(toydb::CATALOG_PAGE_ID);
    page_manager.FlushAllPages();

    std::cout << "Goodbye!" << std::endl;
    return 0;
}
