#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace toydb {

// 通用 ID / 页面常量
const uint32_t INVALID_PAGE_ID = 0; // Represents an invalid or unallocated page ID
const uint32_t CATALOG_PAGE_ID = 1; // Fixed page ID for the Catalog

// 事务 ID 定义（供 MVCC 与日志等组件统一使用）
using TxId = int;
const TxId INVALID_TX_ID = -1;

// 行多版本结构：后续 B+ 树可以直接存储该类型作为 value
struct VersionedRow {
    TxId create_tx{INVALID_TX_ID};   // 创建该版本的事务
    TxId delete_tx{INVALID_TX_ID};   // 标记删除该版本的事务（如果未删除则为 INVALID_TX_ID）
    bool committed{false};           // 该版本是否已经提交
    std::vector<std::string> data;   // 实际的行数据（列值）
};

} // namespace toydb

