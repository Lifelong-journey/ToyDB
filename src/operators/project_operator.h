#pragma once

#include "operator.h"
#include "table.h"
#include <memory>
#include <vector>
#include <string>

namespace toydb {

// 投影算子：选择特定的列
class ProjectOperator : public Operator {
public:
    ProjectOperator(
        std::unique_ptr<Operator> child,
        const std::vector<std::string>& select_columns,
        const std::vector<Column>& table_columns
    );
    
    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    std::unique_ptr<Operator> child_;
    std::vector<std::string> select_columns_;
    std::vector<Column> table_columns_;
    std::vector<size_t> column_indices_; // 要投影的列索引
    
    void BuildColumnIndices();
};

} // namespace toydb

