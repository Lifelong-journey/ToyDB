#pragma once

#include <string>
#include <vector>
#include <optional>
#include "nodes/parsenodes.hpp"
#include "definitions.h"
#include "mvcc.h"
#include "table.h"

namespace toydb {

// Helper to convert PGValue to string for column types, etc.
std::string PgValueToString(duckdb_libpgquery::PGValue* value_node);

// Helper to trim whitespace from a string
std::string Trim(const std::string& str);

// Helper to extract value from PGNode (constant or column reference)
struct WhereValue {
    bool is_column;
    std::string column_name;
    std::string constant_value;
    int constant_int;
    
    WhereValue() : is_column(false), constant_int(0) {}
};

// Extract value from PGNode for WHERE clause evaluation
WhereValue ExtractWhereValue(duckdb_libpgquery::PGNode* node, 
                             const toydb::Table* table);

// Evaluate WHERE clause condition
bool EvaluateWhereClause(duckdb_libpgquery::PGNode* where_node, 
                         const std::vector<std::string>& row,
                         const toydb::Table* table);

// Evaluate JOIN condition for two rows
bool EvaluateJoinCondition(duckdb_libpgquery::PGNode* join_node,
                            const std::vector<std::string>& left_row,
                            const std::vector<std::string>& right_row,
                            const toydb::Table* left_table,
                            const toydb::Table* right_table);

// MVCC 可见性判断：实现快照隔离
bool IsRowVisible(const VersionedRow& ver,
                  const std::optional<int>& session_tx_id,
                  const std::optional<TxId>& snapshot,
                  const MVCCManager& mvcc_manager);

// 从版本链中找到对当前事务可见的最新版本（使用快照隔离）
// 返回指向可见版本的指针，如果不存在则返回 nullptr
const VersionedRow* FindVisibleVersion(
    const std::vector<VersionedRow>& version_chain,
    const std::optional<int>& session_tx_id,
    const std::optional<TxId>& snapshot,
    const MVCCManager& mvcc_manager);

} // namespace toydb

