#include "table_scan_operator.h"
#include "query_utils.h"

namespace toydb {

TableScanOperator::TableScanOperator(
    Table* table,
    std::optional<int> tx_id,
    std::optional<TxId> snapshot,
    MVCCManager& mvcc_manager
) : table_(table), tx_id_(tx_id), snapshot_(snapshot), mvcc_manager_(mvcc_manager),
    current_index_(0), initialized_(false) {
}

void TableScanOperator::Initialize() {
    if (initialized_) {
        return;
    }
    
    if (!table_ || !table_->bptree_) {
        // 表不存在或 B+ 树未初始化
        all_values_.clear();
        initialized_ = true;
        return;
    }
    
    // 获取所有键值对
    all_values_ = table_->bptree_->GetAllValues();
    initialized_ = true;
}

std::optional<Row> TableScanOperator::Next() {
    Initialize();
    
    while (current_index_ < all_values_.size()) {
        const auto& kv = all_values_[current_index_];
        current_index_++;
        
        // 从版本链中找到可见版本
        const VersionedRow* visible = FindVisibleVersion(
            kv.second, tx_id_, snapshot_, mvcc_manager_
        );
        
        if (visible) {
            return visible->data;
        }
    }
    
    return std::nullopt;
}

void TableScanOperator::Reset() {
    current_index_ = 0;
    initialized_ = false;
    all_values_.clear();
}

std::vector<std::string> TableScanOperator::GetOutputColumns() const {
    if (!table_) {
        return {};
    }
    std::vector<std::string> col_names;
    for (const auto& col : table_->GetColumns()) {
        col_names.push_back(col.name);
    }
    return col_names;
}

} // namespace toydb

