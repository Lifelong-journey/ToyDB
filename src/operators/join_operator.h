#pragma once

#include "operator.h"
#include "table.h"
#include <memory>
#include <vector>
#include "nodes/parsenodes.hpp"

namespace toydb {

// JOIN 算子：实现内连接
// 注意：为了简化实现，这里使用嵌套循环连接（Nested Loop Join）
// 对于大表，可以考虑使用 Hash Join 或 Sort-Merge Join
class JoinOperator : public Operator {
public:
    JoinOperator(
        std::unique_ptr<Operator> left_child,
        std::unique_ptr<Operator> right_child,
        duckdb_libpgquery::PGNode* join_condition,
        Table* left_table,
        Table* right_table
    );
    
    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    std::unique_ptr<Operator> left_child_;
    std::unique_ptr<Operator> right_child_;
    duckdb_libpgquery::PGNode* join_condition_;
    Table* left_table_;
    Table* right_table_;
    
    // 嵌套循环连接状态
    Row current_left_row_;
    bool has_current_left_;
    std::vector<Row> right_rows_; // 物化右侧所有行（简化实现）
    size_t right_index_;
    bool right_initialized_;
    
    void InitializeRight();
};

// 展示用的 Merge Join（当前未在执行路径中调用）
class MergeJoinOperator : public Operator {
public:
    MergeJoinOperator(std::unique_ptr<Operator> left_child,
                      std::unique_ptr<Operator> right_child,
                      duckdb_libpgquery::PGNode* join_condition,
                      Table* left_table,
                      Table* right_table);

    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    void Initialize();
    int FindColumnIndex(const std::vector<Column>& cols, const std::string& name) const;

    std::unique_ptr<Operator> left_child_;
    std::unique_ptr<Operator> right_child_;
    duckdb_libpgquery::PGNode* join_condition_;
    Table* left_table_;
    Table* right_table_;

    bool initialized_;
    std::vector<Row> left_rows_;
    std::vector<Row> right_rows_;
    size_t left_idx_;
    size_t right_idx_;
    int left_key_idx_;
    int right_key_idx_;
};

} // namespace toydb

