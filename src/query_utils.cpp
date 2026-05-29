#include "query_utils.h"
#include "postgres_parser.hpp"
#include "table.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <iostream>

namespace toydb {

std::string PgValueToString(duckdb_libpgquery::PGValue* value_node) {
    if (value_node == nullptr) {
        return "";
    }
    if (value_node->type == duckdb_libpgquery::T_PGString) {
        return value_node->val.str;
    }
    // Handle other types if necessary
    return "";
}

std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

WhereValue ExtractWhereValue(duckdb_libpgquery::PGNode* node, const toydb::Table* table) {
    WhereValue result;
    if (node == nullptr) return result;
    
    if (node->type == duckdb_libpgquery::T_PGAConst) {
        duckdb_libpgquery::PGAConst* const_node = (duckdb_libpgquery::PGAConst*)node;
        result.is_column = false;
        if (const_node->val.type == duckdb_libpgquery::T_PGInteger) {
            result.constant_int = const_node->val.val.ival;
            result.constant_value = std::to_string(const_node->val.val.ival);
        } else if (const_node->val.type == duckdb_libpgquery::T_PGString) {
            result.constant_value = const_node->val.val.str;
        }
    } else if (node->type == duckdb_libpgquery::T_PGColumnRef) {
        duckdb_libpgquery::PGColumnRef* col_ref = (duckdb_libpgquery::PGColumnRef*)node;
        result.is_column = true;
        // 对于限定列名（table.col），fields 可能包含多个节点，取最后一个作为列名
        if (col_ref->fields && col_ref->fields->head) {
            auto* field_node = col_ref->fields->head;
            // 找到链表最后一个
            while (field_node->next != nullptr) {
                field_node = field_node->next;
            }
            result.column_name = PgValueToString((duckdb_libpgquery::PGValue*)field_node->data.ptr_value);
        }
    }
    return result;
}

bool EvaluateWhereClause(duckdb_libpgquery::PGNode* where_node, 
                         const std::vector<std::string>& row,
                         const toydb::Table* table) {
    if (where_node == nullptr) return true;
    
    if (where_node->type == duckdb_libpgquery::T_PGAExpr) {
        duckdb_libpgquery::PGAExpr* expr = (duckdb_libpgquery::PGAExpr*)where_node;
        
        if (expr->kind == duckdb_libpgquery::PG_AEXPR_OP) {
            // Comparison operator
            std::string op = PgValueToString((duckdb_libpgquery::PGValue*)expr->name->head->data.ptr_value);
            
            WhereValue left_val = ExtractWhereValue(expr->lexpr, table);
            WhereValue right_val = ExtractWhereValue(expr->rexpr, table);
            
            // Get actual values
            std::string left_str, right_str;
            int left_int = 0, right_int = 0;
            bool left_is_int = false, right_is_int = false;
            
            if (left_val.is_column) {
                // Find column index
                const auto& columns = table->GetColumns();
                size_t col_idx = SIZE_MAX;
                for (size_t i = 0; i < columns.size(); ++i) {
                    if (columns[i].name == left_val.column_name) {
                        col_idx = i;
                        break;
                    }
                }
                if (col_idx < row.size()) {
                    left_str = row[col_idx];
                    try {
                        left_int = std::stoi(left_str);
                        left_is_int = true;
                    } catch (...) {
                        left_is_int = false;
                    }
                }
            } else {
                left_str = left_val.constant_value;
                left_int = left_val.constant_int;
                left_is_int = true;
            }
            
            if (right_val.is_column) {
                const auto& columns = table->GetColumns();
                size_t col_idx = SIZE_MAX;
                for (size_t i = 0; i < columns.size(); ++i) {
                    if (columns[i].name == right_val.column_name) {
                        col_idx = i;
                        break;
                    }
                }
                if (col_idx < row.size()) {
                    right_str = row[col_idx];
                    try {
                        right_int = std::stoi(right_str);
                        right_is_int = true;
                    } catch (...) {
                        right_is_int = false;
                    }
                }
            } else {
                right_str = right_val.constant_value;
                right_int = right_val.constant_int;
                right_is_int = true;
            }
            
            // Compare
            if (op == "=") {
                if (left_is_int && right_is_int) {
                    return left_int == right_int;
                } else {
                    return left_str == right_str;
                }
            } else if (op == "!=" || op == "<>") {
                if (left_is_int && right_is_int) {
                    return left_int != right_int;
                } else {
                    return left_str != right_str;
                }
            } else if (op == ">") {
                if (left_is_int && right_is_int) {
                    return left_int > right_int;
                } else {
                    return left_str > right_str;
                }
            } else if (op == "<") {
                if (left_is_int && right_is_int) {
                    return left_int < right_int;
                } else {
                    return left_str < right_str;
                }
            } else if (op == ">=") {
                if (left_is_int && right_is_int) {
                    return left_int >= right_int;
                } else {
                    return left_str >= right_str;
                }
            } else if (op == "<=") {
                if (left_is_int && right_is_int) {
                    return left_int <= right_int;
                } else {
                    return left_str <= right_str;
                }
            }
        }
    } else if (where_node->type == duckdb_libpgquery::T_PGBoolExpr) {
        // Boolean expression (AND/OR/NOT)
        duckdb_libpgquery::PGBoolExpr* bool_expr = (duckdb_libpgquery::PGBoolExpr*)where_node;
        if (bool_expr->boolop == duckdb_libpgquery::PG_AND_EXPR) {
            // AND: all args must be true
            bool result = true;
            for (auto* arg_node = bool_expr->args->head; arg_node != nullptr; arg_node = arg_node->next) {
                duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)arg_node->data.ptr_value;
                if (!EvaluateWhereClause(node_data, row, table)) {
                    result = false;
                    break;
                }
            }
            return result;
        } else if (bool_expr->boolop == duckdb_libpgquery::PG_OR_EXPR) {
            // OR: at least one arg must be true
            for (auto* arg_node = bool_expr->args->head; arg_node != nullptr; arg_node = arg_node->next) {
                duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)arg_node->data.ptr_value;
                if (EvaluateWhereClause(node_data, row, table)) {
                    return true;
                }
            }
            return false;
        } else if (bool_expr->boolop == duckdb_libpgquery::PG_NOT_EXPR) {
            // NOT: negate the result
            if (bool_expr->args && bool_expr->args->head) {
                duckdb_libpgquery::PGNode* node_data = (duckdb_libpgquery::PGNode*)bool_expr->args->head->data.ptr_value;
                return !EvaluateWhereClause(node_data, row, table);
            }
        }
    }
    
    return true; // Default: include row
}

bool EvaluateJoinCondition(duckdb_libpgquery::PGNode* join_node,
    const std::vector<std::string>& left_row,
    const std::vector<std::string>& right_row,
    const toydb::Table* left_table,
    const toydb::Table* right_table) {
    if (join_node == nullptr) return true;

    // For JOIN conditions, we need to handle column references from both tables
    if (join_node->type == duckdb_libpgquery::T_PGAExpr) {
    duckdb_libpgquery::PGAExpr* expr = (duckdb_libpgquery::PGAExpr*)join_node;
    if (expr->kind == duckdb_libpgquery::PG_AEXPR_OP) {
    std::string op = PgValueToString((duckdb_libpgquery::PGValue*)expr->name->head->data.ptr_value);

    WhereValue left_val = ExtractWhereValue(expr->lexpr, left_table);
    WhereValue right_val = ExtractWhereValue(expr->rexpr, right_table);

    // Helper function to get column value from either table
    auto get_column_value = [&](const WhereValue& val, 
                    bool prefer_left_table) -> 
                std::pair<std::string, bool> {
    std::string str_val;
    bool is_int = false;

    if (val.is_column) {
    const auto& left_columns = left_table->GetColumns();
    const auto& right_columns = right_table->GetColumns();
    size_t col_idx = SIZE_MAX;

    // 根据prefer_left_table决定优先查找的表
    if (prefer_left_table) {
    // 先查左表
    for (size_t i = 0; i < left_columns.size(); ++i) {
        if (left_columns[i].name == val.column_name) {
            col_idx = i;
            break;
        }
    }
    if (col_idx < left_row.size()) {
        str_val = left_row[col_idx];
    } else {
        // 左表没找到，查右表
        for (size_t i = 0; i < right_columns.size(); ++i) {
            if (right_columns[i].name == val.column_name) {
                col_idx = i;
                break;
            }
        }
        if (col_idx < right_row.size()) {
            str_val = right_row[col_idx];
        }
    }
    } else {
    // 先查右表
    for (size_t i = 0; i < right_columns.size(); ++i) {
        if (right_columns[i].name == val.column_name) {
            col_idx = i;
            break;
        }
    }
    if (col_idx < right_row.size()) {
        str_val = right_row[col_idx];
    } else {
        // 右表没找到，查左表
        for (size_t i = 0; i < left_columns.size(); ++i) {
            if (left_columns[i].name == val.column_name) {
                col_idx = i;
                break;
            }
        }
        if (col_idx < left_row.size()) {
            str_val = left_row[col_idx];
        }
    }
    }

    // 尝试转换为整数
    if (!str_val.empty()) {
    try {
        std::stoi(str_val);  // 只是检查是否可转换
        is_int = true;
    } catch (...) {
        is_int = false;
    }
    }
    } else {
    // 常量值
    str_val = val.constant_value;
    is_int = true;
    }

    return {str_val, is_int};
    };

    // 左操作数：优先从左表查找（因为来自left_val）
    auto [left_str, left_is_int] = get_column_value(left_val, true);
    int left_int = 0;
    if (left_is_int && !left_str.empty()) {
    try {
    left_int = std::stoi(left_str);
    } catch (...) {
    left_is_int = false;
    }
    }

    // 右操作数：优先从右表查找（因为来自right_val）← 关键修正！
    auto [right_str, right_is_int] = get_column_value(right_val, false);
    int right_int = 0;
    if (right_is_int && !right_str.empty()) {
    try {
    right_int = std::stoi(right_str);
    } catch (...) {
    right_is_int = false;
    }
    }

    // Compare
    if (op == "=") {
        if (left_is_int && right_is_int) {
        return left_int == right_int;
        } else {
        return left_str == right_str;
    }
    } else if (op == "!=" || op == "<>") {
        if (left_is_int && right_is_int) {
        return left_int != right_int;
        } else {
        return left_str != right_str;
        }
    } else if (op == ">") {
        if (left_is_int && right_is_int) {
        return left_int > right_int;
        } else {
        return left_str > right_str;
        }
    } else if (op == "<") {
        if (left_is_int && right_is_int) {
        return left_int < right_int;
        } else {
        return left_str < right_str;
        }
    } else if (op == ">=") {
        if (left_is_int && right_is_int) {
        return left_int >= right_int;
        } else {
        return left_str >= right_str;
        }
    } else if (op == "<=") {
        if (left_is_int && right_is_int) {
        return left_int <= right_int;
        } else {
        return left_str <= right_str;
        }
    }
    }
    }

return true;
}

bool IsRowVisible(const VersionedRow& ver,
    const std::optional<int>& session_tx_id,
    const std::optional<TxId>& snapshot,
    const MVCCManager& mvcc_manager) {

    // 特殊情况1：非事务查询（如管理员查询）
    if (!session_tx_id.has_value()) {
    return ver.committed && ver.delete_tx == INVALID_TX_ID;
    }

    // 特殊情况2：自己事务的未提交修改
    int current_tx = session_tx_id.value();
    if (ver.create_tx == current_tx) {
    return ver.delete_tx != current_tx;  // 自己创建但未删除的可见
    }

    // 其他所有情况：委托给MVCCManager
    return mvcc_manager.IsVisible(
    current_tx, 
    snapshot.value_or(INVALID_TX_ID),
    ver.create_tx, 
    ver.delete_tx
    );
}

const VersionedRow* FindVisibleVersion(
    const std::vector<VersionedRow>& version_chain,
    const std::optional<int>& session_tx_id,
    const std::optional<TxId>& snapshot,
    const MVCCManager& mvcc_manager) {
    // 从最新版本（链尾）开始向前查找，找到第一个可见的版本
    for (auto it = version_chain.rbegin(); it != version_chain.rend(); ++it) {
        if (IsRowVisible(*it, session_tx_id, snapshot, mvcc_manager)) {
            return &(*it);
        }
    }
    return nullptr; // 没有可见版本
}

} // namespace toydb
