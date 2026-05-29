#include "sort_operator.h"

namespace toydb {

SortOperator::SortOperator(
    std::unique_ptr<Operator> child,
    CompareFunc compare_func
) : child_(std::move(child)), compare_func_(compare_func),
    current_index_(0), initialized_(false) {
}

void SortOperator::Initialize() {
    if (initialized_) {
        return;
    }
    
    // 物化所有输入行
    child_->Reset();
    while (true) {
        auto row = child_->Next();
        if (!row.has_value()) {
            break;
        }
        sorted_rows_.push_back(row.value());
    }
    
    // 排序
    std::sort(sorted_rows_.begin(), sorted_rows_.end(), compare_func_);
    initialized_ = true;
}

std::optional<Row> SortOperator::Next() {
    Initialize();
    
    if (current_index_ < sorted_rows_.size()) {
        return sorted_rows_[current_index_++];
    }
    
    return std::nullopt;
}

void SortOperator::Reset() {
    current_index_ = 0;
    initialized_ = false;
    sorted_rows_.clear();
}

std::vector<std::string> SortOperator::GetOutputColumns() const {
    return child_->GetOutputColumns();
}

} // namespace toydb

