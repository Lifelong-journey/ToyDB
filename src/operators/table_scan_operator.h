#pragma once

#include "operator.h"
#include "table.h"
#include "b_plus_tree.h"
#include "mvcc.h"
#include <optional>

namespace toydb {

// 表扫描算子：从表中逐行读取数据
class TableScanOperator : public Operator {
public:
    TableScanOperator(
        Table* table,
        std::optional<int> tx_id,
        std::optional<TxId> snapshot,
        MVCCManager& mvcc_manager
    );
    
    std::optional<Row> Next() override;
    void Reset() override;
    std::vector<std::string> GetOutputColumns() const override;

private:
    Table* table_;
    std::optional<int> tx_id_;
    std::optional<TxId> snapshot_;
    MVCCManager& mvcc_manager_;
    
    // 迭代器状态
    std::vector<std::pair<int, std::vector<VersionedRow>>> all_values_;
    size_t current_index_;
    bool initialized_;
    
    void Initialize();
};

} // namespace toydb

