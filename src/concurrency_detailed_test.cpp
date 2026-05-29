#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <set>
#include <atomic>
#include <mutex>
#include <sstream>
#include <fstream>
#include <cassert>

#include "lock_manager.h"
#include "log_manager.h"
#include "page_manager.h"
#include "catalog.h"
#include "table.h"
#include "b_plus_tree.h"
#include "mvcc.h"
#include "query_utils.h"

using namespace std::chrono_literals;

namespace toydb {

// 简单断言工具（避免与 PostgreSQL 的 Assert 宏冲突）
void TestAssert(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "[FAIL] " << msg << std::endl;
        std::exit(1);
    }
}

// 测试1: 快照隔离 - 读事务看不到并发写事务的修改
void TestSnapshotIsolationReadWrite() {
    std::cout << "Running TestSnapshotIsolationReadWrite..." << std::endl;

    toydb::PageManager page_manager("test_snapshot.db");
    toydb::Catalog catalog;
    toydb::LogManager log_manager("test_snapshot.log");
    toydb::MVCCManager mvcc_manager;
    toydb::LockManager lock_manager;

    // 创建表
    std::vector<Column> columns;
    columns.emplace_back("id", ColumnType::INT, 0);
    columns.emplace_back("v", ColumnType::INT, 0);
    auto table = std::make_unique<Table>("test", std::move(columns), page_manager);
    catalog.AddTable(std::move(table));

    // 插入初始数据
    int tx_init = log_manager.BeginTransaction();
    toydb::TxId snap_init = mvcc_manager.BeginSnapshot(tx_init);
    toydb::Table* t = catalog.GetTable("test");
    
    toydb::VersionedRow row;
    row.data = {"1", "10"};
    row.create_tx = tx_init;
    row.delete_tx = toydb::INVALID_TX_ID;
    row.committed = false;
    std::vector<toydb::VersionedRow> chain = {row};
    t->bptree_->Insert(1, chain);
    log_manager.LogInsert(tx_init, "test", 1, row.data);
    log_manager.CommitTransaction(tx_init, page_manager);
    mvcc_manager.OnTransactionCommitted(tx_init);

    // 读事务开始（获取快照）
    int tx_read = log_manager.BeginTransaction();
    toydb::TxId snap_read = mvcc_manager.BeginSnapshot(tx_read);

    // 写事务插入新数据
    int tx_write = log_manager.BeginTransaction();
    toydb::TxId snap_write = mvcc_manager.BeginSnapshot(tx_write);
    
    toydb::VersionedRow row2;
    row2.data = {"2", "20"};
    row2.create_tx = tx_write;
    row2.delete_tx = toydb::INVALID_TX_ID;
    row2.committed = false;
    std::vector<toydb::VersionedRow> chain2 = {row2};
    t->bptree_->Insert(2, chain2);
    log_manager.LogInsert(tx_write, "test", 2, row2.data);
    log_manager.CommitTransaction(tx_write, page_manager);
    mvcc_manager.OnTransactionCommitted(tx_write);

    // 读事务应该看不到写事务插入的数据（因为快照在写事务提交之前）
    std::vector<toydb::VersionedRow> read_chain;
    bool found = t->bptree_->Search(2, read_chain);
    if (found) {
        const toydb::VersionedRow* visible = toydb::FindVisibleVersion(read_chain, tx_read, snap_read, mvcc_manager);
        TestAssert(visible == nullptr, "Read transaction should not see data inserted after snapshot");
    }

    log_manager.RollbackTransaction(tx_read, catalog, page_manager);
    mvcc_manager.OnTransactionRolledBack(tx_read);

    std::cout << "TestSnapshotIsolationReadWrite passed." << std::endl;
}

// 测试2: 写-写冲突检测
void TestWriteWriteConflict() {
    std::cout << "Running TestWriteWriteConflict..." << std::endl;

    toydb::LockManager lock_manager;

    int tx1 = 10;
    int tx2 = 11;

    std::atomic<bool> tx1_acquired(false);
    std::atomic<bool> tx2_blocked(false);
    std::atomic<bool> tx2_acquired(false);

    std::thread t1([&] {
        bool ok = lock_manager.AcquireRowLock("t", 1, tx1, toydb::LockMode::WRITE);
        TestAssert(ok, "TX1 failed to acquire WRITE lock");
        tx1_acquired = true;
        std::this_thread::sleep_for(200ms);
        lock_manager.ReleaseLocks(tx1);
    });

    std::thread t2([&] {
        // 等待 tx1 获得锁
        while (!tx1_acquired) {
            std::this_thread::sleep_for(10ms);
        }
        std::this_thread::sleep_for(50ms); // 确保 tx1 已经持有锁

        auto start = std::chrono::steady_clock::now();
        bool ok = lock_manager.AcquireRowLock("t", 1, tx2, toydb::LockMode::WRITE);
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        TestAssert(ok, "TX2 failed to acquire WRITE lock");
        TestAssert(elapsed >= 100, "TX2 should be blocked by TX1's WRITE lock");
        tx2_acquired = true;
        lock_manager.ReleaseLocks(tx2);
    });

    t1.join();
    t2.join();

    TestAssert(tx1_acquired && tx2_acquired, "Both transactions should complete");
    std::cout << "TestWriteWriteConflict passed." << std::endl;
}

// 测试3: 多读单写
void TestMultipleReadersSingleWriter() {
    std::cout << "Running TestMultipleReadersSingleWriter..." << std::endl;

    toydb::LockManager lock_manager;

    int writer = 20;
    std::vector<int> readers = {21, 22, 23};
    std::atomic<int> readers_acquired(0);
    std::atomic<bool> writer_acquired(false);
    std::atomic<bool> writer_released(false);

    // 启动多个读事务
    std::vector<std::thread> reader_threads;
    for (int reader : readers) {
        reader_threads.emplace_back([&, reader] {
            bool ok = lock_manager.AcquireRowLock("t", 1, reader, toydb::LockMode::READ);
            TestAssert(ok, "Reader failed to acquire READ lock");
            readers_acquired++;
            // 等待主线程确认写事务被阻塞
            std::this_thread::sleep_for(150ms);
            // 释放读锁，让写事务能够获得锁
            lock_manager.ReleaseLocks(reader);
        });
    }

    // 等待所有读事务获得锁
    while (readers_acquired < (int)readers.size()) {
        std::this_thread::sleep_for(10ms);
    }

    // 写事务尝试获取锁（应该被阻塞）
    std::thread writer_thread([&] {
        auto start = std::chrono::steady_clock::now();
        bool ok = lock_manager.AcquireRowLock("t", 1, writer, toydb::LockMode::WRITE);
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        TestAssert(ok, "Writer failed to acquire WRITE lock");
        TestAssert(elapsed >= 100, "Writer should be blocked until readers release locks");
        writer_acquired = true;
        std::this_thread::sleep_for(50ms);
        lock_manager.ReleaseLocks(writer);
        writer_released = true;
    });

    // 等待一段时间，确保写事务被阻塞
    std::this_thread::sleep_for(100ms);
    TestAssert(!writer_acquired, "Writer should be blocked by readers");

    // 等待读线程释放锁（读线程会在 150ms 后释放）
    for (auto& t : reader_threads) {
        t.join();
    }

    // 写事务现在应该能获得锁
    writer_thread.join();
    TestAssert(writer_acquired, "Writer should acquire lock after readers release");

    std::cout << "TestMultipleReadersSingleWriter passed." << std::endl;
}

// 测试4: 版本链并发访问
void TestVersionChainConcurrency() {
    std::cout << "Running TestVersionChainConcurrency..." << std::endl;

    toydb::PageManager page_manager("test_version.db");
    toydb::Catalog catalog;
    toydb::LogManager log_manager("test_version.log");
    toydb::MVCCManager mvcc_manager;

    // 创建表
    std::vector<toydb::Column> columns;
    columns.emplace_back("id", toydb::ColumnType::INT, 0);
    columns.emplace_back("v", toydb::ColumnType::INT, 0);
    auto table = std::make_unique<toydb::Table>("test", std::move(columns), page_manager);
    catalog.AddTable(std::move(table));

    toydb::Table* t = catalog.GetTable("test");

    // 创建初始版本
    int tx1 = log_manager.BeginTransaction();
    toydb::TxId snap1 = mvcc_manager.BeginSnapshot(tx1);
    toydb::VersionedRow v1;
    v1.data = {"1", "10"};
    v1.create_tx = tx1;
    v1.delete_tx = toydb::INVALID_TX_ID;
    v1.committed = false;
    std::vector<toydb::VersionedRow> chain = {v1};
    t->bptree_->Insert(1, chain);
    log_manager.LogInsert(tx1, "test", 1, v1.data);
    log_manager.CommitTransaction(tx1, page_manager);
    mvcc_manager.OnTransactionCommitted(tx1);

    // 创建第二个版本
    int tx2 = log_manager.BeginTransaction();
    toydb::TxId snap2 = mvcc_manager.BeginSnapshot(tx2);
    std::vector<toydb::VersionedRow> existing_chain;
    t->bptree_->Search(1, existing_chain);
    toydb::VersionedRow v2;
    v2.data = {"1", "20"};
    v2.create_tx = tx2;
    v2.delete_tx = toydb::INVALID_TX_ID;
    v2.committed = false;
    existing_chain.push_back(v2);
    t->bptree_->Delete(1);
    t->bptree_->Insert(1, existing_chain);
    log_manager.LogUpdate(tx2, "test", 1, v1.data, v2.data);
    log_manager.CommitTransaction(tx2, page_manager);
    mvcc_manager.OnTransactionCommitted(tx2);

    // 读事务（使用第一个快照）应该看到第一个版本
    std::vector<toydb::VersionedRow> read_chain;
    t->bptree_->Search(1, read_chain);
    const toydb::VersionedRow* visible = toydb::FindVisibleVersion(read_chain, tx1, snap1, mvcc_manager);
    TestAssert(visible != nullptr, "Should find visible version");
    TestAssert(visible->data[1] == "10", "Should see first version with value 10");

    // 新读事务（使用第二个快照）应该看到第二个版本
    int tx3 = log_manager.BeginTransaction();
    toydb::TxId snap3 = mvcc_manager.BeginSnapshot(tx3);
    const toydb::VersionedRow* visible2 = toydb::FindVisibleVersion(read_chain, tx3, snap3, mvcc_manager);
    TestAssert(visible2 != nullptr, "Should find visible version");
    TestAssert(visible2->data[1] == "20", "Should see second version with value 20");
    log_manager.RollbackTransaction(tx3, catalog, page_manager);

    std::cout << "TestVersionChainConcurrency passed." << std::endl;
}

// 测试5: 事务提交序列
void TestTransactionCommitSequence() {
    std::cout << "Running TestTransactionCommitSequence..." << std::endl;

    toydb::MVCCManager mvcc_manager;

    // 先提交事务 1-4
    for (int i = 1; i <= 4; ++i) {
        mvcc_manager.OnTransactionCommitted(i);
    }

    // 在事务 5 开始时创建快照（此时只有事务 1-4 已提交）
    int tx_snapshot = 5;
    toydb::TxId snap = mvcc_manager.BeginSnapshot(tx_snapshot);
    TestAssert(snap >= 4, "Snapshot should include transactions up to snapshot time");

    // 然后提交事务 5-10
    for (int i = 5; i <= 10; ++i) {
        mvcc_manager.OnTransactionCommitted(i);
    }

    // 验证可见性：事务 3 在快照之前已提交，应该可见
    bool visible = mvcc_manager.IsVisible(6, snap, 3, toydb::INVALID_TX_ID);
    TestAssert(visible, "Transaction 3 should be visible to snapshot taken at tx 5");

    // 验证可见性：事务 7 在快照之后提交，不应该可见
    bool not_visible = mvcc_manager.IsVisible(6, snap, 7, toydb::INVALID_TX_ID);
    TestAssert(!not_visible, "Transaction 7 should not be visible to snapshot taken at tx 5");

    std::cout << "TestTransactionCommitSequence passed." << std::endl;
}

// 测试6: 锁升级
void TestLockUpgrade() {
    std::cout << "Running TestLockUpgrade..." << std::endl;

    toydb::LockManager lock_manager;

    int tx = 30;

    // 先获取读锁
    bool ok1 = lock_manager.AcquireRowLock("t", 1, tx, toydb::LockMode::READ);
    TestAssert(ok1, "Failed to acquire READ lock");

    // 尝试升级为写锁（当没有其他持有者时应该成功）
    bool ok2 = lock_manager.AcquireRowLock("t", 1, tx, toydb::LockMode::WRITE);
    TestAssert(ok2, "Failed to upgrade READ lock to WRITE lock");

    lock_manager.ReleaseLocks(tx);

    std::cout << "TestLockUpgrade passed." << std::endl;
}

// 测试7: 表级锁与行级锁的层次关系
void TestTableRowLockHierarchy() {
    std::cout << "Running TestTableRowLockHierarchy..." << std::endl;

    toydb::LockManager lock_manager;

    int tx_table = 40;
    int tx_row = 41;

    std::atomic<bool> table_acquired(false);
    std::atomic<bool> row_blocked(false);

    // 获取表级写锁
    std::thread t1([&] {
        bool ok = lock_manager.AcquireTableLock("t", tx_table, toydb::LockMode::WRITE);
        TestAssert(ok, "Failed to acquire table WRITE lock");
        table_acquired = true;
        std::this_thread::sleep_for(200ms);
        lock_manager.ReleaseLocks(tx_table);
    });

    // 尝试获取行级读锁（应该被阻塞）
    std::thread t2([&] {
        while (!table_acquired) {
            std::this_thread::sleep_for(10ms);
        }
        std::this_thread::sleep_for(50ms);

        auto start = std::chrono::steady_clock::now();
        bool ok = lock_manager.AcquireRowLock("t", 1, tx_row, toydb::LockMode::READ);
        auto end = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        TestAssert(ok, "Failed to acquire row READ lock");
        TestAssert(elapsed >= 100, "Row lock should be blocked by table lock");
        row_blocked = true;
        lock_manager.ReleaseLocks(tx_row);
    });

    t1.join();
    t2.join();

    TestAssert(table_acquired && row_blocked, "Both locks should be acquired");
    std::cout << "TestTableRowLockHierarchy passed." << std::endl;
}

} // namespace toydb

int main() {
    std::cout << "=== Running detailed concurrency tests ===" << std::endl;

    try {
        toydb::TestSnapshotIsolationReadWrite();
        toydb::TestWriteWriteConflict();
        toydb::TestMultipleReadersSingleWriter();
        toydb::TestVersionChainConcurrency();
        toydb::TestTransactionCommitSequence();
        toydb::TestLockUpgrade();
        toydb::TestTableRowLockHierarchy();

        std::cout << "=== All detailed concurrency tests passed ===" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}

