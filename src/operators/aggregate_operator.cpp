#include "aggregate_operator.h"
#include <algorithm>
#include <limits>
#include <cmath>

namespace toydb {

AggregateOperator::AggregateOperator(
    std::unique_ptr<Operator> child,
    AggregateType agg_type,
    const std::string& column_name,
    const std::vector<Column>& table_columns
) : child_(std::move(child)), agg_type_(agg_type), column_name_(column_name),
    table_columns_(table_columns), column_index_(SIZE_MAX), computed_(false) {
    
    // 对 COUNT(*) 等无列名或空列名的场景，统一视为 "*"
    if (column_name_.empty()) {
        column_name_ = "*";
    }

    // 查找列索引
    if (column_name_ != "*") {
        for (size_t i = 0; i < table_columns_.size(); ++i) {
            if (table_columns_[i].name == column_name_) {
                column_index_ = i;
                break;
            }
        }
    }
}

double AggregateOperator::GetNumericValue(const Row& row, size_t col_idx) const {
    if (col_idx >= row.size()) {
        return 0.0;
    }
    try {
        return std::stod(row[col_idx]);
    } catch (...) {
        return 0.0;
    }
}

void AggregateOperator::ComputeAggregate() {
    if (computed_) {
        return;
    }
    
    std::vector<Row> all_rows;
    child_->Reset();
    while (true) {
        auto row = child_->Next();
        if (!row.has_value()) {
            break;
        }
        all_rows.push_back(row.value());
    }
    
    std::string result;
    
    switch (agg_type_) {
        case AggregateType::COUNT:
            if (column_name_ == "*") {
                result = std::to_string(all_rows.size());
            } else {
                size_t count = 0;
                for (const auto& row : all_rows) {
                    if (column_index_ < row.size() && !row[column_index_].empty()) {
                        count++;
                    }
                }
                result = std::to_string(count);
            }
            break;
            
        case AggregateType::SUM:
            if (column_index_ < table_columns_.size()) {
                double sum = 0.0;
                for (const auto& row : all_rows) {
                    sum += GetNumericValue(row, column_index_);
                }
                result = std::to_string(sum);
            }
            break;
            
        case AggregateType::AVG:
            if (column_index_ < table_columns_.size() && !all_rows.empty()) {
                double sum = 0.0;
                for (const auto& row : all_rows) {
                    sum += GetNumericValue(row, column_index_);
                }
                double avg = sum / all_rows.size();
                result = std::to_string(avg);
            }
            break;
            
        case AggregateType::MAX:
            if (column_index_ < table_columns_.size() && !all_rows.empty()) {
                double max_val = std::numeric_limits<double>::lowest();
                for (const auto& row : all_rows) {
                    double val = GetNumericValue(row, column_index_);
                    if (val > max_val) {
                        max_val = val;
                    }
                }
                result = std::to_string(max_val);
            }
            break;
            
        case AggregateType::MIN:
            if (column_index_ < table_columns_.size() && !all_rows.empty()) {
                double min_val = std::numeric_limits<double>::max();
                for (const auto& row : all_rows) {
                    double val = GetNumericValue(row, column_index_);
                    if (val < min_val) {
                        min_val = val;
                    }
                }
                result = std::to_string(min_val);
            }
            break;
    }
    
    result_row_ = {result};
    computed_ = true;
}

std::optional<Row> AggregateOperator::Next() {
    ComputeAggregate();
    
    if (computed_ && !result_row_.empty()) {
        Row ret = result_row_;
        result_row_.clear(); // 只返回一次
        return ret;
    }
    
    return std::nullopt;
}

void AggregateOperator::Reset() {
    computed_ = false;
    result_row_.clear();
    child_->Reset();
}

std::vector<std::string> AggregateOperator::GetOutputColumns() const {
    return {"aggregate_result"};
}

} // namespace toydb

