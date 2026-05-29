#include "lock_manager.h"

#include <iostream>
#include <algorithm>

namespace toydb {

bool LockManager::IsConflict(LockMode existing, LockMode requested) {


    auto idx = [](LockMode m) -> int {
        switch (m) {
            case LockMode::INTENTION_SHARED:    return 0;
            case LockMode::INTENTION_EXCLUSIVE: return 1;
            case LockMode::READ:                return 2;
            case LockMode::WRITE:               return 3;
        }
        return 3;
    };

    static const bool compatible[4][4] = {
        // req:   IS     IX     S      X
        /* IS */ {true,  true,  true,  false},
        /* IX */ {true,  true,  false, false},
        /* S  */ {true,  false, true,  false},
        /* X  */ {false, false, false, false}
    };

    int e = idx(existing);
    int r = idx(requested);
    return !compatible[e][r];
}

bool LockManager::TryUpgradeLock(ResourceState& state, const LockRequest& req) {
    auto it = std::find_if(state.holders.begin(), state.holders.end(),
                           [&](const LockRequest& h) { return h.tx_id == req.tx_id; });
    if (it == state.holders.end()) {
        return false;
    }
    if (it->mode == LockMode::WRITE) {
        return true;
    }
    bool other_holders = std::any_of(state.holders.begin(), state.holders.end(),
                                     [&](const LockRequest& h) { return h.tx_id != req.tx_id; });
    if (!other_holders) {
        it->mode = LockMode::WRITE;
    return true;
    }
    return false;
}

bool LockManager::CheckAndGrant(const std::string& resource, const LockRequest& req) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto& state_ptr = resource_states_[resource];
    if (!state_ptr) {
        state_ptr = std::make_shared<ResourceState>();
    }
    std::shared_ptr<ResourceState> state = state_ptr;

    auto has_conflict = [&](const ResourceState& s) {
        for (const auto& h : s.holders) {
        if (h.tx_id == req.tx_id) {
                continue;
            }
            if (IsConflict(h.mode, req.mode)) {
                return true;
            }
        }
            return false;
    };

    while (true) {
        auto existing_it = std::find_if(state->holders.begin(), state->holders.end(),
                                        [&](const LockRequest& h) { return h.tx_id == req.tx_id; });
        if (existing_it != state->holders.end()) {
            // 已持有的锁与本次请求一起必须仍然与其他事务锁兼容，否则不能简单重入/升级
            // 对于行/表锁，我们只在没有其他事务持有冲突锁时允许重入或升级。
            if (existing_it->mode == LockMode::WRITE || req.mode == LockMode::READ) {
                if (!has_conflict(*state)) {
                    return true;
                }
            }
            if (req.mode == LockMode::WRITE && TryUpgradeLock(*state, req)) {
                if (!has_conflict(*state)) {
                    return true;
                }
            }
        }

        if (!has_conflict(*state)) {
            state->holders.push_back(req);
    resources_by_tx_[req.tx_id].push_back(resource);

            const char* mode_str = nullptr;
            switch (req.mode) {
                case LockMode::INTENTION_SHARED:    mode_str = "IS"; break;
                case LockMode::INTENTION_EXCLUSIVE: mode_str = "IX"; break;
                case LockMode::READ:                mode_str = "S";  break;
                case LockMode::WRITE:               mode_str = "X";  break;
            }
    std::cout << "LockManager: TX " << req.tx_id << " acquired "
                      << mode_str << " lock on " << resource << std::endl;
    return true;
        }

        // 存在冲突：阻塞等待其他事务释放锁
        state->cv.wait(lock);
    }
}

bool LockManager::AcquireTableLock(const std::string& table_name, int tx_id, LockMode mode) {
    if (tx_id <= 0) {
        // 非事务语句暂不加锁
        return true;
    }
    std::string resource = "table:" + table_name;
    LockRequest req{tx_id, table_name, std::nullopt, mode};
    return CheckAndGrant(resource, req);
}

bool LockManager::AcquireRowLock(const std::string& table_name, int key, int tx_id, LockMode mode) {
    if (tx_id <= 0) {
        // 非事务语句暂不加锁
        return true;
    }
    
    
    std::string table_resource = "table:" + table_name;
    std::string row_resource = "table:" + table_name + ":row:" + std::to_string(key);
    
    // 确定需要的表级意向锁类型
    LockMode table_intention_mode;
    LockMode row_lock_mode;
    if (mode == LockMode::READ) {
        table_intention_mode = LockMode::INTENTION_SHARED;  // IS
        row_lock_mode = LockMode::READ;  // S
    } else if (mode == LockMode::WRITE) {
        table_intention_mode = LockMode::INTENTION_EXCLUSIVE;  // IX
        row_lock_mode = LockMode::WRITE;  // X
    } else {
        // 如果已经是意向锁，直接使用
        table_intention_mode = mode;
        row_lock_mode = mode;
    }
    
    // 注意：表级资源上的 X 锁会与 IS 锁冲突，表级资源上的 S 锁会与 IX 锁冲突
    // CheckAndGrant 会检查表级资源上的冲突并阻塞，实现多粒度锁协议
    LockRequest table_req{tx_id, table_name, std::nullopt, table_intention_mode};
    if (!CheckAndGrant(table_resource, table_req)) {
        return false;
    }
    
    LockRequest row_req{tx_id, table_name, key, row_lock_mode};
    if (!CheckAndGrant(row_resource, row_req)) {
        // 如果行锁获取失败，表级意向锁仍然保留（事务可能还需要其他行锁）
        // 在事务回滚时会统一释放
        return false;
    }
    
    return true;
}

void LockManager::ReleaseLocks(int tx_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = resources_by_tx_.find(tx_id);
    if (it == resources_by_tx_.end()) {
        return;
    }
    for (const auto& resource : it->second) {
        auto state_it = resource_states_.find(resource);
        if (state_it == resource_states_.end() || !state_it->second) {
            continue;
        }
        auto& holders = state_it->second->holders;
        holders.erase(
            std::remove_if(holders.begin(), holders.end(),
                           [tx_id](const LockRequest& r) { return r.tx_id == tx_id; }),
            holders.end());
        state_it->second->cv.notify_all();
    }
    resources_by_tx_.erase(it);
    std::cout << "LockManager: released locks for TX " << tx_id << std::endl;
}

} // namespace toydb

