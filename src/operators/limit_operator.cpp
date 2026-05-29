#include "limit_operator.h"

namespace toydb {

LimitOperator::LimitOperator(
    std::unique_ptr<Operator> child,
    size_t limit
) : child_(std::move(child)), limit_(limit), count_(0) {
}

std::optional<Row> LimitOperator::Next() {
    if (count_ >= limit_) {
        return std::nullopt;
    }
    
    auto row = child_->Next();
    if (row.has_value()) {
        count_++;
    }
    
    return row;
}

void LimitOperator::Reset() {
    count_ = 0;
    child_->Reset();
}

std::vector<std::string> LimitOperator::GetOutputColumns() const {
    return child_->GetOutputColumns();
}

} // namespace toydb

