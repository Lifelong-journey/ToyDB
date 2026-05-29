#include "mvcc.h"
#include <algorithm>

namespace toydb {

TxId MVCCManager::BeginSnapshot(TxId tx_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    // 快照：返回当前最大已提交事务ID
    // 对于快照隔离，我们记录所有已提交的事务ID
    // 简化实现：返回最大提交序列号对应的最大事务ID
    if (committed_tx_set_.empty()) {
        return INVALID_TX_ID; // 没有已提交的事务
    }
    // 这里我们使用最大事务ID作为快照标识
    TxId max_tx = *std::max_element(committed_tx_set_.begin(), committed_tx_set_.end());
    return max_tx;
}

bool MVCCManager::IsVisible(TxId current_tx, TxId snapshot, TxId create_tx, TxId delete_tx) const {
    // 系统内部行（如通过回滚恢复的）没有创建事务，默认总是可见
    if (create_tx == INVALID_TX_ID) {
        // 但需要检查是否被删除
        if (delete_tx != INVALID_TX_ID) {
            // 检查删除事务是否在快照之前已提交
            std::lock_guard<std::mutex> guard(mutex_);
            if (delete_tx == current_tx) {
                // 当前事务自己删除的，不可见（除非回滚）
                return false;
            }
            // 删除事务必须在快照之前已提交，且不在快照中
            if (committed_tx_set_.count(delete_tx) > 0 && delete_tx <= snapshot) {
                return false; // 删除事务在快照时已提交，不可见
            }
        }
        return true;
    }

    // 自己事务创建的版本，对自己可见
    if (current_tx != INVALID_TX_ID && create_tx == current_tx) {
        // 但需要检查是否被删除
        if (delete_tx != INVALID_TX_ID && delete_tx == current_tx) {
            return false; // 当前事务自己删除的，不可见
        }
        return true;
    }

    std::lock_guard<std::mutex> guard(mutex_);


    bool create_committed = committed_tx_set_.count(create_tx) > 0 && create_tx <= snapshot;
    if (!create_committed) {
        return false; // 创建事务在快照时未提交，不可见
    }

    // 检查删除事务
    if (delete_tx != INVALID_TX_ID) {
        if (delete_tx == current_tx) {
            return false; 
        }
        // 删除事务必须在快照之后提交（delete_tx > snapshot），或者未提交
        bool delete_committed = committed_tx_set_.count(delete_tx) > 0;
        if (delete_committed && delete_tx <= snapshot) {
            return false; // 删除事务在快照时已提交，不可见
        }
    }

    return true; // 版本在快照时已创建且未被删除，可见
}

void MVCCManager::OnTransactionCommitted(TxId tx_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    committed_tx_set_.insert(tx_id);
    tx_commit_seq_[tx_id] = next_commit_seq_++;
}

void MVCCManager::OnTransactionRolledBack(TxId tx_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    // 回滚的事务不需要从 committed_tx_set_ 中移除（因为它本来就不在）
    // 但我们可以清理 commit_seq 映射（如果存在）
    tx_commit_seq_.erase(tx_id);
}

TxId MVCCManager::GetMaxCommittedTxId() const {
    std::lock_guard<std::mutex> guard(mutex_);
    if (committed_tx_set_.empty()) {
        return INVALID_TX_ID;
    }
    return *std::max_element(committed_tx_set_.begin(), committed_tx_set_.end());
}

} // namespace toydb



