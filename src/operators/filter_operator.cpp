#include "filter_operator.h"
#include "query_utils.h"

namespace toydb {

FilterOperator::FilterOperator(
    std::unique_ptr<Operator> child,
    duckdb_libpgquery::PGNode* where_clause,
    Table* table
) : child_(std::move(child)), where_clause_(where_clause), table_(table) {
}

std::optional<Row> FilterOperator::Next() {
    while (true) {
        auto row = child_->Next();
        if (!row.has_value()) {
            return std::nullopt;
        }
        
        // 评估 WHERE 条件
        if (!where_clause_ || EvaluateWhereClause(where_clause_, row.value(), table_)) {
            return row;
        }
    }
}

void FilterOperator::Reset() {
    child_->Reset();
}

std::vector<std::string> FilterOperator::GetOutputColumns() const {
    return child_->GetOutputColumns();
}

} // namespace toydb

