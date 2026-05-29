#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <set>
#include <cassert>
#include <atomic>

#include "lock_manager.h"
#include "log_manager.h"
#include "page_manager.h"
#include "catalog.h"
#include "b_plus_tree.h"

using namespace std::chrono_literals;

namespace toydb {

// 简单断言工具，失败时输出消息并终止
void Assert(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        std::exit(1);
    }
}

void TestSharedReadLocks(LockManager& lock_manager) {
    std::cout << "Running TestSharedReadLocks..." << std::endl;

    int tx1 = 1;
    int tx2 = 2;

    std::thread t1([&] {
        bool ok = lock_manager.AcquireRowLock("t", 1, tx1, LockMode::READ);
        Assert(ok, "TX1 failed to acquire READ lock");
        std::this_thread::sleep_for(200ms);
        lock_manager.ReleaseLocks(tx1);
    });

    std::thread t2([&] {
        std::this_thread::sleep_for(20ms); // 确保 t1 先获得锁
        auto start = std::chrono::steady_clock::now();
        bool ok = lock_manager.AcquireRowLock("t", 1, tx2, LockMode::READ);
        auto end = std::chrono::steady_clock::now();
        Assert(ok, "TX2 failed to acquire READ lock");
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        // 如果读读之间仍然被阻塞，elapsed 会接近 200ms，这里期望它很小
        Assert(elapsed_ms < 100, "Shared READ locks appear to be blocking unexpectedly");
        lock_manager.ReleaseLocks(tx2);
    });

    t1.join();
    t2.join();

    std::cout << "TestSharedReadLocks passed." << std::endl;
}

void TestExclusiveWriteLocks(LockManager& lock_manager) {
    std::cout << "Running TestExclusiveWriteLocks..." << std::endl;

    int tx1 = 3;
    int tx2 = 4;

    std::thread t1([&] {
        bool ok = lock_manager.AcquireRowLock("t", 1, tx1, LockMode::WRITE);
        Assert(ok, "TX1 failed to acquire WRITE lock");
        std::this_thread::sleep_for(300ms); // 持有一段时间，观察另一个事务是否会阻塞
        lock_manager.ReleaseLocks(tx1);
    });

    std::thread t2([&] {
        std::this_thread::sleep_for(50ms); // 确保 t1 已经拿到写锁
        auto start = std::chrono::steady_clock::now();
        bool ok = lock_manager.AcquireRowLock("t", 1, tx2, LockMode::WRITE);
        auto end = std::chrono::steady_clock::now();
        Assert(ok, "TX2 failed to acquire WRITE lock");
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        // 如果锁是阻塞的，TX2 至少要等到 TX1 释放锁（约 300ms）
        Assert(elapsed_ms >= 250, "WRITE lock did not block as expected on conflicting WRITE");
        lock_manager.ReleaseLocks(tx2);
    });

    t1.join();
    t2.join();

    std::cout << "TestExclusiveWriteLocks passed." << std::endl;
}

// 表级 X 锁应阻塞后续的行级 S/X 锁（通过意向锁实现）
void TestTableXBlocksRowLocks(LockManager& lock_manager) {
    std::cout << "Running TestTableXBlocksRowLocks..." << std::endl;

    int tx1 = 5;
    int tx2 = 6;

    std::thread t1([&] {
        bool ok = lock_manager.AcquireTableLock("t", tx1, LockMode::WRITE); // 表级 X
        Assert(ok, "TX1 failed to acquire table WRITE lock");
        std::this_thread::sleep_for(300ms);
        lock_manager.ReleaseLocks(tx1);
    });

    std::thread t2([&] {
        std::this_thread::sleep_for(50ms); // 确保 TX1 先持有表级 X
        auto start = std::chrono::steady_clock::now();
        // 行级 READ => 需要表级 IS，与表级 X 不兼容，应阻塞到 TX1 释放
        bool ok = lock_manager.AcquireRowLock("t", 1, tx2, LockMode::READ);
        auto end = std::chrono::steady_clock::now();
        Assert(ok, "TX2 failed to acquire row READ lock under table X");
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        Assert(elapsed_ms >= 250, "Row READ lock was not blocked by table WRITE lock as expected");
        lock_manager.ReleaseLocks(tx2);
    });

    t1.join();
    t2.join();

    std::cout << "TestTableXBlocksRowLocks passed." << std::endl;
}

void TestLogManagerConcurrentBegin(LogManager& log_manager, PageManager& page_manager) {
    std::cout << "Running TestLogManagerConcurrentBegin..." << std::endl;

    const int kThreads = 4;
    std::vector<int> tx_ids(kThreads, 0);
    std::vector<std::thread> threads;

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            tx_ids[i] = log_manager.BeginTransaction();
            // 立即提交，主要目的是验证并发分配 TX ID 没有冲突或崩溃
            bool ok = log_manager.CommitTransaction(tx_ids[i], page_manager);
            Assert(ok, "CommitTransaction failed for tx " + std::to_string(tx_ids[i]));
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    std::set<int> unique_ids(tx_ids.begin(), tx_ids.end());
    Assert(unique_ids.size() == tx_ids.size(), "Duplicate transaction IDs detected in concurrent BeginTransaction");

    std::cout << "TestLogManagerConcurrentBegin passed." << std::endl;
}

void TestBPlusTreeConcurrentInsertAndSearch() {
    std::cout << "Running TestBPlusTreeConcurrentInsertAndSearch..." << std::endl;

    toydb::PageManager page_manager("toydb_bpt_concurrent.db");
    toydb::BPlusTree<int, std::vector<std::string>> tree(page_manager, toydb::INVALID_PAGE_ID);

    const int kThreads = 4;
    const int kPerThread = 100;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &tree]() {
            int base = t * 1000;
            for (int i = 0; i < 100; ++i) {
                int key = base + i;
                std::vector<std::string> row = {std::to_string(key)};
                tree.Insert(key, row);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // Verify all keys are present
    for (int t = 0; t < kThreads; ++t) {
        int base = t * 1000;
        for (int i = 0; i < 100; ++i) {
            int key = base + i;
            std::vector<std::string> row;
            bool found = tree.Search(key, row);
            Assert(found, "BPlusTree missing key " + std::to_string(key) + " after concurrent inserts");
            Assert(!row.empty() && row[0] == std::to_string(key),
                   "BPlusTree returned wrong value for key " + std::to_string(key));
        }
    }

    std::cout << "TestBPlusTreeConcurrentInsertAndSearch passed." << std::endl;
}

} // namespace toydb

int main() {
    std::cout << "=== Running concurrency tests ===" << std::endl;

    toydb::LockManager lock_manager;
    toydb::PageManager page_manager("toydb_concurrent.db");
    toydb::LogManager log_manager("toydb_concurrent.log");

    toydb::TestSharedReadLocks(lock_manager);
    toydb::TestExclusiveWriteLocks(lock_manager);
    toydb::TestTableXBlocksRowLocks(lock_manager);
    toydb::TestLogManagerConcurrentBegin(log_manager, page_manager);
    toydb::TestBPlusTreeConcurrentInsertAndSearch();

    std::cout << "=== All concurrency tests passed ===" << std::endl;
    return 0;
}


