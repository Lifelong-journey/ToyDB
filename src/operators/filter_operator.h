#pragma once

#include "operator.h"
#include "table.h"
#include <memory>
#include "nodes/parsenodes.hpp"

namespace toydb {

// 过滤算子：根据 WHERE 条件过滤行
class FilterOperator : public Operator {
public:
    FilterOperator(
        std::unique_ptr<Operator> child,
        duckdb_libpgquery::PGNode* where_clause,
        Table* table
    );
    
    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    std::unique_ptr<Operator> child_;
    duckdb_libpgquery::PGNode* where_clause_;
    Table* table_;
};

} // namespace toydb

