#include "join_operator.h"
#include "query_utils.h"
#include <algorithm>

namespace toydb {

JoinOperator::JoinOperator(
    std::unique_ptr<Operator> left_child,
    std::unique_ptr<Operator> right_child,
    duckdb_libpgquery::PGNode* join_condition,
    Table* left_table,
    Table* right_table
) : left_child_(std::move(left_child)), right_child_(std::move(right_child)),
    join_condition_(join_condition), left_table_(left_table), right_table_(right_table),
    has_current_left_(false), right_index_(0), right_initialized_(false) {
}

void JoinOperator::InitializeRight() {
    if (right_initialized_) {
        return;
    }
    
    // 物化右侧所有行（简化实现，可以优化为流式处理）
    right_child_->Reset();
    while (true) {
        auto row = right_child_->Next();
        if (!row.has_value()) {
            break;
        }
        right_rows_.push_back(row.value());
    }
    right_initialized_ = true;
}

std::optional<Row> JoinOperator::Next() {
    InitializeRight();
    
    while (true) {
        // 获取左侧下一行（如果需要）
        if (!has_current_left_) {
            auto left_row = left_child_->Next();
            if (!left_row.has_value()) {
                return std::nullopt;
            }
            current_left_row_ = left_row.value();
            has_current_left_ = true;
            right_index_ = 0; // 重置右侧索引
        }
        
        // 遍历右侧所有行
        while (right_index_ < right_rows_.size()) {
            const auto& right_row = right_rows_[right_index_];
            right_index_++;
            
            // 组合左右行
            Row combined_row = current_left_row_;
            combined_row.insert(combined_row.end(), right_row.begin(), right_row.end());
            
            // 评估连接条件
            if (!join_condition_ || EvaluateJoinCondition(
                join_condition_, current_left_row_, right_row, left_table_, right_table_)) {
                return combined_row;
            }
        }
        
        // 右侧遍历完毕，获取左侧下一行
        has_current_left_ = false;
    }
}

void JoinOperator::Reset() {
    left_child_->Reset();
    right_child_->Reset();
    has_current_left_ = false;
    right_index_ = 0;
    right_initialized_ = false;
    right_rows_.clear();
    current_left_row_.clear();
}

std::vector<std::string> JoinOperator::GetOutputColumns() const {
    std::vector<std::string> cols;
    if (left_table_) {
        for (const auto& col : left_table_->GetColumns()) {
            cols.push_back(col.name);
        }
    }
    if (right_table_) {
        for (const auto& col : right_table_->GetColumns()) {
            cols.push_back(col.name);
        }
    }
    return cols;
}


MergeJoinOperator::MergeJoinOperator(
    std::unique_ptr<Operator> left_child,
    std::unique_ptr<Operator> right_child,
    duckdb_libpgquery::PGNode* join_condition,
    Table* left_table,
    Table* right_table
) : left_child_(std::move(left_child)),
    right_child_(std::move(right_child)),
    join_condition_(join_condition),
    left_table_(left_table),
    right_table_(right_table),
    initialized_(false),
    left_idx_(0),
    right_idx_(0),
    left_key_idx_(-1),
    right_key_idx_(-1) {}

int MergeJoinOperator::FindColumnIndex(const std::vector<Column>& cols, const std::string& name) const {
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

void MergeJoinOperator::Initialize() {
    if (initialized_) return;

    // 物化左右
    left_child_->Reset();
    right_child_->Reset();
    while (auto r = left_child_->Next()) {
        left_rows_.push_back(r.value());
    }
    while (auto r = right_child_->Next()) {
        right_rows_.push_back(r.value());
    }

    // 尝试从 join_condition 中推断等值列，默认为首列
    std::string left_col_name;
    std::string right_col_name;
    if (join_condition_ && join_condition_->type == duckdb_libpgquery::T_PGAExpr) {
        auto* expr = (duckdb_libpgquery::PGAExpr*)join_condition_;
        if (expr->kind == duckdb_libpgquery::PG_AEXPR_OP && expr->name && expr->name->head) {
            std::string op = PgValueToString((duckdb_libpgquery::PGValue*)expr->name->head->data.ptr_value);
            if (op == "=") {
                auto l = ExtractWhereValue(expr->lexpr, left_table_);
                auto r = ExtractWhereValue(expr->rexpr, right_table_);
                if (l.is_column && r.is_column) {
                    left_col_name = l.column_name;
                    right_col_name = r.column_name;
                }
            }
        }
    }
    if (left_col_name.empty() && left_table_) left_col_name = left_table_->GetColumns().empty() ? "" : left_table_->GetColumns()[0].name;
    if (right_col_name.empty() && right_table_) right_col_name = right_table_->GetColumns().empty() ? "" : right_table_->GetColumns()[0].name;

    left_key_idx_ = left_table_ ? FindColumnIndex(left_table_->GetColumns(), left_col_name) : -1;
    right_key_idx_ = right_table_ ? FindColumnIndex(right_table_->GetColumns(), right_col_name) : -1;

    auto key_getter = [](const Row& row, int idx) -> std::string {
        if (idx < 0 || static_cast<size_t>(idx) >= row.size()) return "";
        return row[idx];
    };

    // 按键排序
    std::sort(left_rows_.begin(), left_rows_.end(), [&](const Row& a, const Row& b) {
        return key_getter(a, left_key_idx_) < key_getter(b, left_key_idx_);
    });
    std::sort(right_rows_.begin(), right_rows_.end(), [&](const Row& a, const Row& b) {
        return key_getter(a, right_key_idx_) < key_getter(b, right_key_idx_);
    });

    initialized_ = true;
    left_idx_ = 0;
    right_idx_ = 0;
}

std::optional<Row> MergeJoinOperator::Next() {
    Initialize();

    auto key_getter = [](const Row& row, int idx) -> std::string {
        if (idx < 0 || static_cast<size_t>(idx) >= row.size()) return "";
        return row[idx];
    };

    while (left_idx_ < left_rows_.size() && right_idx_ < right_rows_.size()) {
        const auto& lrow = left_rows_[left_idx_];
        const auto& rrow = right_rows_[right_idx_];
        std::string lkey = key_getter(lrow, left_key_idx_);
        std::string rkey = key_getter(rrow, right_key_idx_);

        if (lkey == rkey) {
            Row combined = lrow;
            combined.insert(combined.end(), rrow.begin(), rrow.end());
            ++right_idx_;
            // 用 join_condition_ 再做一次判断，保证与语义一致
            if (!join_condition_ || EvaluateJoinCondition(join_condition_, lrow, rrow, left_table_, right_table_)) {
                return combined;
            }
        } else if (lkey < rkey) {
            ++left_idx_;
            right_idx_ = 0; // 重新从右侧开头匹配
        } else {
            ++right_idx_;
        }
    }

    return std::nullopt;
}

void MergeJoinOperator::Reset() {
    left_child_->Reset();
    right_child_->Reset();
    initialized_ = false;
    left_rows_.clear();
    right_rows_.clear();
    left_idx_ = right_idx_ = 0;
}

std::vector<std::string> MergeJoinOperator::GetOutputColumns() const {
    std::vector<std::string> cols;
    if (left_table_) {
        for (const auto& c : left_table_->GetColumns()) cols.push_back(c.name);
    }
    if (right_table_) {
        for (const auto& c : right_table_->GetColumns()) cols.push_back(c.name);
    }
    return cols;
}

} // namespace toydb

