#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <optional>
#include <condition_variable>
#include <mutex>
#include <memory>

namespace toydb {

// 锁模式：支持层次化锁（意向锁 + S/X）
// READ  = S  共享锁
// WRITE = X  排它锁
enum class LockMode {
    INTENTION_SHARED,   // IS
    INTENTION_EXCLUSIVE,// IX
    READ,               // S
    WRITE               // X
};

// 粒度：表级锁与行级锁（主键）
struct LockRequest {
    int tx_id;
    std::string table_name;
    std::optional<int> key; // 空 => 表级锁；有值 => 该表中某一主键行
    LockMode mode;
};

// 简单的 2PL 锁管理器：支持多读单写，不做等待队列，冲突直接报错
class LockManager {
public:
    // 申请表级锁（READ/WRITE）
    bool AcquireTableLock(const std::string& table_name, int tx_id, LockMode mode);

    // 申请行级锁（按主键）
    bool AcquireRowLock(const std::string& table_name, int key, int tx_id, LockMode mode);

    // 在事务提交或回滚时释放锁
    void ReleaseLocks(int tx_id);

private:
    struct ResourceState {
        std::vector<LockRequest> holders;
        std::condition_variable cv;
    };

    // 按资源维护锁：资源键 = "table" 或 "table:key"
    std::unordered_map<std::string, std::shared_ptr<ResourceState>> resource_states_;
    // 按事务维护其持有的锁列表
    std::unordered_map<int, std::vector<std::string>> resources_by_tx_;
    mutable std::mutex mutex_;

    bool CheckAndGrant(const std::string& resource, const LockRequest& req);
    static bool IsConflict(LockMode existing, LockMode requested);
    bool TryUpgradeLock(ResourceState& state, const LockRequest& req);
};

} // namespace toydb


