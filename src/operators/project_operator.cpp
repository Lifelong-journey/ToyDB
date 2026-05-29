#include "project_operator.h"

namespace toydb {

ProjectOperator::ProjectOperator(
    std::unique_ptr<Operator> child,
    const std::vector<std::string>& select_columns,
    const std::vector<Column>& table_columns
) : child_(std::move(child)), select_columns_(select_columns), table_columns_(table_columns) {
    BuildColumnIndices();
}

void ProjectOperator::BuildColumnIndices() {
    if (select_columns_.empty()) {
        // SELECT * - 投影所有列
        for (size_t i = 0; i < table_columns_.size(); ++i) {
            column_indices_.push_back(i);
        }
    } else {
        // 根据列名找到索引
        for (const auto& col_name : select_columns_) {
            for (size_t i = 0; i < table_columns_.size(); ++i) {
                if (table_columns_[i].name == col_name) {
                    column_indices_.push_back(i);
                    break;
                }
            }
        }
    }
}

std::optional<Row> ProjectOperator::Next() {
    auto row = child_->Next();
    if (!row.has_value()) {
        return std::nullopt;
    }
    
    Row projected_row;
    for (size_t idx : column_indices_) {
        if (idx < row->size()) {
            projected_row.push_back((*row)[idx]);
        }
    }
    
    return projected_row;
}

void ProjectOperator::Reset() {
    child_->Reset();
}

std::vector<std::string> ProjectOperator::GetOutputColumns() const {
    if (select_columns_.empty()) {
        std::vector<std::string> col_names;
        for (const auto& col : table_columns_) {
            col_names.push_back(col.name);
        }
        return col_names;
    }
    return select_columns_;
}

} // namespace toydb

