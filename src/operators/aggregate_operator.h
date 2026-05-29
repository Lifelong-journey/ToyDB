#pragma once

#include "operator.h"
#include "table.h"
#include <memory>
#include <string>
#include <vector>

namespace toydb {

// 聚合算子：实现聚合函数（COUNT, SUM, AVG, MAX, MIN）
// 注意：需要物化所有输入行才能聚合
class AggregateOperator : public Operator {
public:
    enum class AggregateType {
        COUNT,
        SUM,
        AVG,
        MAX,
        MIN
    };
    
    AggregateOperator(
        std::unique_ptr<Operator> child,
        AggregateType agg_type,
        const std::string& column_name,
        const std::vector<Column>& table_columns
    );
    
    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    std::unique_ptr<Operator> child_;
    AggregateType agg_type_;
    std::string column_name_;
    std::vector<Column> table_columns_;
    size_t column_index_;
    bool computed_;
    Row result_row_;
    
    void ComputeAggregate();
    double GetNumericValue(const Row& row, size_t col_idx) const;
};

} // namespace toydb

