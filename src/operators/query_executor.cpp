#include "query_executor.h"
#include "table_scan_operator.h"
#include "filter_operator.h"
#include "project_operator.h"
#include "join_operator.h"
#include "sort_operator.h"
#include "aggregate_operator.h"
#include "limit_operator.h"
#include "query_utils.h"
#include "b_plus_tree.h" // For ToString
#include <iostream>
#include <algorithm>
#include <cstring>

namespace toydb {

QueryExecutor::QueryExecutor(
    Catalog& catalog,
    MVCCManager& mvcc_manager,
    std::optional<int> tx_id,
    std::optional<TxId> snapshot
) : catalog_(catalog), mvcc_manager_(mvcc_manager), tx_id_(tx_id), snapshot_(snapshot) {
}

std::vector<std::string> QueryExecutor::ParseSelectColumns(duckdb_libpgquery::PGSelectStmt* select_stmt) {
    std::vector<std::string> select_columns;
    bool is_select_all = false;
    
    for (auto* target_node = select_stmt->targetList->head; target_node != nullptr; target_node = target_node->next) {
        duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)target_node->data.ptr_value;
        if (node_data->type == duckdb_libpgquery::T_PGResTarget) {
            duckdb_libpgquery::PGResTarget* res_target = (duckdb_libpgquery::PGResTarget*)node_data;
            if (res_target->val) {
                if (res_target->val->type == duckdb_libpgquery::T_PGAStar) {
                    is_select_all = true;
                    break;
                } else if (res_target->val->type == duckdb_libpgquery::T_PGColumnRef) {
                    duckdb_libpgquery::PGColumnRef* col_ref = (duckdb_libpgquery::PGColumnRef*)res_target->val;
                    if (col_ref->fields && col_ref->fields->head) {
                        select_columns.push_back(PgValueToString((duckdb_libpgquery::PGValue*)col_ref->fields->head->data.ptr_value));
                    }
                }
            }
        }
    }
    
    if (is_select_all || (!select_columns.empty() && select_columns[0].empty())) {
        select_columns.clear();
    }
    
    return select_columns;
}

bool QueryExecutor::HasAggregate(duckdb_libpgquery::PGSelectStmt* select_stmt) {
    for (auto* target_node = select_stmt->targetList->head; target_node != nullptr; target_node = target_node->next) {
        duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)target_node->data.ptr_value;
        if (node_data->type == duckdb_libpgquery::T_PGResTarget) {
            duckdb_libpgquery::PGResTarget* res_target = (duckdb_libpgquery::PGResTarget*)node_data;
            if (res_target->val && res_target->val->type == duckdb_libpgquery::T_PGFuncCall) {
                return true;
            }
        }
    }
    return false;
}

std::pair<std::string, std::string> QueryExecutor::ParseAggregate(duckdb_libpgquery::PGSelectStmt* select_stmt) {
    for (auto* target_node = select_stmt->targetList->head; target_node != nullptr; target_node = target_node->next) {
        duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)target_node->data.ptr_value;
        if (node_data->type == duckdb_libpgquery::T_PGResTarget) {
            duckdb_libpgquery::PGResTarget* res_target = (duckdb_libpgquery::PGResTarget*)node_data;
            if (res_target->val && res_target->val->type == duckdb_libpgquery::T_PGFuncCall) {
                duckdb_libpgquery::PGFuncCall* func_call = (duckdb_libpgquery::PGFuncCall*)res_target->val;
                std::string func_name;
                std::string column_name;
                
                if (func_call->funcname && func_call->funcname->head) {
                    func_name = PgValueToString((duckdb_libpgquery::PGValue*)func_call->funcname->head->data.ptr_value);
                    std::transform(func_name.begin(), func_name.end(), func_name.begin(), ::tolower);
                }
                
                if (func_call->args && func_call->args->head) {
                    duckdb_libpgquery::PGNode* arg_node = (duckdb_libpgquery::PGNode*)func_call->args->head->data.ptr_value;
                    if (arg_node->type == duckdb_libpgquery::T_PGColumnRef) {
                        duckdb_libpgquery::PGColumnRef* col_ref = (duckdb_libpgquery::PGColumnRef*)arg_node;
                        if (col_ref->fields && col_ref->fields->head) {
                            column_name = PgValueToString((duckdb_libpgquery::PGValue*)col_ref->fields->head->data.ptr_value);
                        }
                    } else if (arg_node->type == duckdb_libpgquery::T_PGAStar) {
                        column_name = "*";
                    }
                }
                
                return {func_name, column_name};
            }
        }
    }
    return {"", ""};
}

SortOperator::CompareFunc QueryExecutor::BuildSortCompareFunc(
    duckdb_libpgquery::PGSelectStmt* select_stmt,
    const std::vector<Column>& output_columns
) {
    if (!select_stmt->sortClause || output_columns.empty()) {
        // 默认排序：按第一列升序
        return [](const Row& a, const Row& b) {
            if (a.empty() || b.empty()) return false;
            try {
                double a_val = std::stod(a[0]);
                double b_val = std::stod(b[0]);
                return a_val < b_val;
            } catch (...) {
                return a[0] < b[0];
            }
        };
    }
    
    // 解析 ORDER BY 子句
    auto* sort_node = (duckdb_libpgquery::PGNode*)select_stmt->sortClause->head->data.ptr_value;
    if (sort_node->type == duckdb_libpgquery::T_PGSortBy) {
        duckdb_libpgquery::PGSortBy* sort_by = (duckdb_libpgquery::PGSortBy*)sort_node;
        size_t col_idx = SIZE_MAX;
        
        if (sort_by->node && sort_by->node->type == duckdb_libpgquery::T_PGColumnRef) {
            duckdb_libpgquery::PGColumnRef* col_ref = (duckdb_libpgquery::PGColumnRef*)sort_by->node;
            if (col_ref->fields && col_ref->fields->head) {
                std::string col_name = PgValueToString((duckdb_libpgquery::PGValue*)col_ref->fields->head->data.ptr_value);
                for (size_t i = 0; i < output_columns.size(); ++i) {
                    if (output_columns[i].name == col_name) {
                        col_idx = i;
                        break;
                    }
                }
            }
        }
        
        bool ascending = (sort_by->sortby_dir == duckdb_libpgquery::PG_SORTBY_ASC || 
                         sort_by->sortby_dir == duckdb_libpgquery::PG_SORTBY_DEFAULT);
        
        return [col_idx, ascending](const Row& a, const Row& b) {
            if (col_idx >= a.size() || col_idx >= b.size()) {
                return false;
            }
            try {
                double a_val = std::stod(a[col_idx]);
                double b_val = std::stod(b[col_idx]);
                return ascending ? (a_val < b_val) : (a_val > b_val);
            } catch (...) {
                return ascending ? (a[col_idx] < b[col_idx]) : (a[col_idx] > b[col_idx]);
            }
        };
    }
    
    // 默认排序
    return [](const Row& a, const Row& b) {
        if (a.empty() || b.empty()) return false;
        try {
            double a_val = std::stod(a[0]);
            double b_val = std::stod(b[0]);
            return a_val < b_val;
        } catch (...) {
            return a[0] < b[0];
        }
    };
}

std::unique_ptr<Operator> QueryExecutor::BuildQueryPlan(duckdb_libpgquery::PGSelectStmt* select_stmt) {
    // 检查是否是 JOIN 查询
    bool is_join = false;
    Table* left_table = nullptr;
    Table* right_table = nullptr;
    duckdb_libpgquery::PGNode* join_condition = nullptr;
    std::string left_table_name, right_table_name;
    
    if (select_stmt->fromClause && select_stmt->fromClause->head) {
        duckdb_libpgquery::PGNode* from_node = (duckdb_libpgquery::PGNode*)select_stmt->fromClause->head->data.ptr_value;
        
        if (from_node->type == duckdb_libpgquery::T_PGJoinExpr) {
            is_join = true;
            duckdb_libpgquery::PGJoinExpr* join_expr = (duckdb_libpgquery::PGJoinExpr*)from_node;
            
            if (join_expr->larg && join_expr->larg->type == duckdb_libpgquery::T_PGRangeVar) {
                duckdb_libpgquery::PGRangeVar* left_range = (duckdb_libpgquery::PGRangeVar*)join_expr->larg;
                left_table_name = left_range->relname;
                left_table = catalog_.GetTable(left_table_name);
            }
            
            if (join_expr->rarg && join_expr->rarg->type == duckdb_libpgquery::T_PGRangeVar) {
                duckdb_libpgquery::PGRangeVar* right_range = (duckdb_libpgquery::PGRangeVar*)join_expr->rarg;
                right_table_name = right_range->relname;
                right_table = catalog_.GetTable(right_table_name);
            }
            
            join_condition = join_expr->quals;
        } else if (from_node->type == duckdb_libpgquery::T_PGRangeVar) {
            duckdb_libpgquery::PGRangeVar* range_var = (duckdb_libpgquery::PGRangeVar*)from_node;
            left_table_name = range_var->relname;
            left_table = catalog_.GetTable(left_table_name);
        }
    }
    
    std::unique_ptr<Operator> root;
    
    // Helper to append post-processing：WHERE -> AGG/SORT/PROJECT -> LIMIT
    auto apply_post = [&](std::unique_ptr<Operator> node, const std::vector<Column>& cols) {
        if (!node) return node;
        if (select_stmt->whereClause) {
            node = std::make_unique<FilterOperator>(std::move(node), select_stmt->whereClause, left_table);
        }
        if (HasAggregate(select_stmt)) {
            auto [func_name, col_name] = ParseAggregate(select_stmt);
            AggregateOperator::AggregateType agg_type;
            if (func_name == "count")      agg_type = AggregateOperator::AggregateType::COUNT;
            else if (func_name == "sum")   agg_type = AggregateOperator::AggregateType::SUM;
            else if (func_name == "avg")   agg_type = AggregateOperator::AggregateType::AVG;
            else if (func_name == "max")   agg_type = AggregateOperator::AggregateType::MAX;
            else if (func_name == "min")   agg_type = AggregateOperator::AggregateType::MIN;
            else                           agg_type = AggregateOperator::AggregateType::COUNT;
            node = std::make_unique<AggregateOperator>(std::move(node), agg_type, col_name, cols);
        } else {
            if (select_stmt->sortClause) {
                auto compare_func = BuildSortCompareFunc(select_stmt, cols);
                node = std::make_unique<SortOperator>(std::move(node), compare_func);
            }
            auto select_columns = ParseSelectColumns(select_stmt);
            node = std::make_unique<ProjectOperator>(std::move(node), select_columns, cols);
        }
        if (select_stmt->limitCount && select_stmt->limitCount->type == duckdb_libpgquery::T_PGAConst) {
            auto* const_node = (duckdb_libpgquery::PGAConst*)select_stmt->limitCount;
            if (const_node->val.type == duckdb_libpgquery::T_PGInteger) {
                size_t limit = const_node->val.val.ival;
                node = std::make_unique<LimitOperator>(std::move(node), limit);
            }
        }
        return node;
    };

    if (is_join && left_table && right_table) {
        auto left_scan = std::make_unique<TableScanOperator>(left_table, tx_id_, snapshot_, mvcc_manager_);
        auto right_scan = std::make_unique<TableScanOperator>(right_table, tx_id_, snapshot_, mvcc_manager_);
        std::vector<Column> combined_columns = left_table->GetColumns();
        const auto& rcols = right_table->GetColumns();
        combined_columns.insert(combined_columns.end(), rcols.begin(), rcols.end());
        
        root = std::make_unique<JoinOperator>(
            std::move(left_scan),
            std::move(right_scan),
            join_condition,
            left_table,
            right_table
        );

        return apply_post(std::move(root), combined_columns);
    } else if (is_join) {
        // JOIN 查询但表不存在
        if (!left_table && !left_table_name.empty()) {
            std::cerr << "Error: Table '" << left_table_name << "' not found." << std::endl;
        }
        if (!right_table && !right_table_name.empty()) {
            std::cerr << "Error: Table '" << right_table_name << "' not found." << std::endl;
        }
        return nullptr;
    } else if (left_table) {
        // 单表查询
        root = std::make_unique<TableScanOperator>(left_table, tx_id_, snapshot_, mvcc_manager_);
    } else {
        // 表不存在
        if (!left_table_name.empty()) {
            std::cerr << "Error: Table '" << left_table_name << "' not found." << std::endl;
        } else {
            std::cerr << "Error: Table not found." << std::endl;
        }
        return nullptr;
    }
    
    return apply_post(std::move(root), left_table->GetColumns());
}

void QueryExecutor::ExecuteAndPrint(std::unique_ptr<Operator> root) {
    if (!root) {
        std::cout << "0 rows" << std::endl;
        return;
    }
    
    size_t row_count = 0;
    while (true) {
        auto row = root->Next();
        if (!row.has_value()) {
            break;
        }
        std::cout << toydb::ToString(row.value()) << std::endl;
        row_count++;
    }
    
    if (row_count == 0) {
        std::cout << "0 rows" << std::endl;
    }
}

} // namespace toydb

