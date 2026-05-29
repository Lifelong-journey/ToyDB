#pragma once

#include "operator.h"
#include "table.h"
#include "catalog.h"
#include "mvcc.h"
#include "sort_operator.h"
#include "nodes/parsenodes.hpp"
#include <memory>
#include <optional>
#include <vector>
#include <string>

namespace toydb {

// 查询执行器：负责构建和执行算子树
class QueryExecutor {
public:
    QueryExecutor(
        Catalog& catalog,
        MVCCManager& mvcc_manager,
        std::optional<int> tx_id,
        std::optional<TxId> snapshot
    );
    
    // 执行 SELECT 查询，返回算子树
    std::unique_ptr<Operator> BuildQueryPlan(duckdb_libpgquery::PGSelectStmt* select_stmt);
    
    // 执行查询并输出结果
    void ExecuteAndPrint(std::unique_ptr<Operator> root);

private:
    Catalog& catalog_;
    MVCCManager& mvcc_manager_;
    std::optional<int> tx_id_;
    std::optional<TxId> snapshot_;
    
    // 辅助方法：解析 SELECT 列
    std::vector<std::string> ParseSelectColumns(duckdb_libpgquery::PGSelectStmt* select_stmt);
    
    // 辅助方法：检查是否有聚合函数
    bool HasAggregate(duckdb_libpgquery::PGSelectStmt* select_stmt);
    
    // 辅助方法：解析聚合函数
    std::pair<std::string, std::string> ParseAggregate(duckdb_libpgquery::PGSelectStmt* select_stmt);
    
    // 辅助方法：构建排序比较函数（基于输出列元数据）
    SortOperator::CompareFunc BuildSortCompareFunc(
        duckdb_libpgquery::PGSelectStmt* select_stmt,
        const std::vector<Column>& output_columns
    );
};

} // namespace toydb

