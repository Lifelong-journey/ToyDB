#pragma once

#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include "definitions.h"

namespace toydb {

// MVCC 管理器：实现快照隔离（Snapshot Isolation）
// 维护全局事务提交序列和快照管理
class MVCCManager {
public:
    MVCCManager() : next_commit_seq_(1) {}

    // 在事务开始时创建一个快照
    // 返回快照ID（实际上是最大已提交的事务ID）
    TxId BeginSnapshot(TxId tx_id);

    // 判断某个版本对当前事务（使用快照）是否可见
    // current_tx: 当前事务ID
    // snapshot: 当前事务的快照（最大已提交事务ID）
    // create_tx: 版本创建事务ID
    // delete_tx: 版本删除事务ID（如果未删除则为 INVALID_TX_ID）
    bool IsVisible(TxId current_tx, TxId snapshot, TxId create_tx, TxId delete_tx) const;

    // 通知事务已提交
    void OnTransactionCommitted(TxId tx_id);

    // 通知事务已回滚
    void OnTransactionRolledBack(TxId tx_id);

    // 获取当前最大已提交事务ID（用于快照）
    TxId GetMaxCommittedTxId() const;

private:
    mutable std::mutex mutex_; // 保护 committed_tx_set_ 和 next_commit_seq_
    std::unordered_set<TxId> committed_tx_set_; // 已提交的事务集合
    TxId next_commit_seq_; // 下一个提交序列号（用于排序）
    std::unordered_map<TxId, TxId> tx_commit_seq_; // 事务ID到提交序列号的映射
};

} // namespace toydb



