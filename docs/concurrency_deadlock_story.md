### ToyDB 并发测试死锁案例记录

> 本文记录在实现 MVCC 与锁管理后，第一次跑并发测试时遇到的一个“多读单写”死锁问题，以及最终的修复过程。  
> 代码和时间点均以当前版本为准（`concurrency_detailed_test.cpp` / `lock_manager.cpp`）。

---

### 一、背景：并发与锁管理的设计

- **锁管理器 `LockManager`**
  - 支持表级 / 行级锁以及意向锁：`INTENTION_SHARED (IS)`, `INTENTION_EXCLUSIVE (IX)`, `READ (S)`, `WRITE (X)`。
  - 通过 `std::mutex + std::condition_variable` 实现阻塞等待：
    - `CheckAndGrant()` 中，如果当前请求与已有持有者冲突，则在线程上调用 `state->cv.wait(lock)` 阻塞，等待有锁释放后被 `notify_all()` 唤醒。
  - 释放锁 `ReleaseLocks(tx_id)` 时，会删除该事务在所有资源上的持有，并调用 `cv.notify_all()`，唤醒等待的事务。

- **并发测试 `concurrency_detailed_test.cpp`**
  - 增强版的并发测试程序，包含多个场景：
    - `TestSnapshotIsolationReadWrite`
    - `TestWriteWriteConflict`
    - `TestMultipleReadersSingleWriter`
    - `TestVersionChainConcurrency`
    - `TestTransactionCommitSequence`
    - `TestLockUpgrade`
    - `TestTableRowLockHierarchy`

本次死锁问题就发生在 **`TestMultipleReadersSingleWriter`（多读单写）** 这个测试里。

---

### 二、最初的测试代码：多读单写（存在死锁）

原始版本的 `TestMultipleReadersSingleWriter`（伪代码简化）大致如下：

```cpp
void TestMultipleReadersSingleWriter() {
    LockManager lock_manager;

    int writer = 20;
    std::vector<int> readers = {21, 22, 23};
    std::atomic<int> readers_acquired(0);
    std::atomic<bool> writer_acquired(false);
    std::atomic<bool> writer_released(false);

    // 启动多个读事务
    for (int reader : readers) {
        reader_threads.emplace_back([&, reader] {
            bool ok = lock_manager.AcquireRowLock("t", 1, reader, READ);
            readers_acquired++;
            // 等待写事务尝试获取锁
            while (!writer_released) {
                std::this_thread::sleep_for(10ms);
            }
            lock_manager.ReleaseLocks(reader);
        });
    }

    // 等待所有读事务获得锁
    while (readers_acquired < readers.size()) { /* busy wait */ }

    // 写事务尝试获取锁（应该被阻塞）
    std::thread writer_thread([&] {
        auto start = now();
        bool ok = lock_manager.AcquireRowLock("t", 1, writer, WRITE);
        auto elapsed = now() - start;
        writer_acquired = true;
        lock_manager.ReleaseLocks(writer);
        writer_released = true;
    });

    // 等待一段时间，确保写事务被阻塞
    std::this_thread::sleep_for(100ms);
    ASSERT(!writer_acquired);

    // 释放所有读锁
    join(reader_threads);
    join(writer_thread);
}
```

**设计意图：**

- 先启动多个读事务拿到 `S` 锁。
- 再启动一个写事务请求 `X` 锁，预期会被阻塞。
- 等待一段时间后验证写事务确实被阻塞，然后释放读锁，最终写事务应该能拿到 `X` 锁。

**实际行为：死锁 / 卡住**

运行 `concurrency_detailed_test` 时，输出停在：

```text
=== Running detailed concurrency tests ===
Running TestMultipleReadersSingleWriter...
（然后长时间没有任何进展）
```

这说明测试线程之间发生了循环等待。

---

### 三、死锁根因分析

关键的死锁点在于 **读线程等待 `writer_released`，而写线程又被读锁阻塞**：

1. **读线程逻辑（简化）：**
   ```cpp
   AcquireRowLock(S);
   readers_acquired++;
   while (!writer_released) {
       sleep(10ms);
   }
   ReleaseLocks(reader);
   ```

2. **写线程逻辑（简化）：**
   ```cpp
   // 当前所有读线程都已经持有 S 锁
   AcquireRowLock(X);  // 被 S 锁阻塞
   writer_acquired = true;
   ReleaseLocks(writer);
   writer_released = true;
   ```

3. **锁管理器逻辑（简化）：**
   - 当持有 `S` 时，新的 `X` 请求与现有 `S` 冲突：
     - 在 `CheckAndGrant()` 中，`has_conflict()` 返回 `true`。
     - 写线程在 `state->cv.wait(lock)` 中阻塞，等待有持锁者释放锁并 `notify_all()`。
   - 但读线程在释放锁前，会一直 `while (!writer_released)` 忙等。

于是形成了一个典型的“循环等待”：

- 写线程需要读线程先释放锁，自己才能获得 `X` 锁并设置 `writer_released = true`。
- 读线程又在等待 `writer_released == true` 后才释放锁。

**结论：**

- 这是**测试代码自身的逻辑死锁**，并非 `LockManager` 或数据库内核实现的问题。

---

### 四、修复方案：改变测试的等待方式

修复思路：  
**不要让读线程等待写线程的状态**，而是：

- 读线程：拿到 `S` 锁后，在一个固定时间后主动释放锁。
- 主线程：在读线程持锁期间，检查写线程是否“确实被阻塞了一段时间”；之后等读线程释放锁，再等写线程成功拿到锁。

最终版本的 `TestMultipleReadersSingleWriter`（核心部分）修改为：

```cpp
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
            // 保持一段时间，让写事务有机会被阻塞
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
```

**关键变化：**

- 删掉了读线程中的：
  ```cpp
  while (!writer_released) { sleep(10ms); }
  ```
  避免读线程依赖写线程的状态。
- 改为：
  - 读线程睡眠一个固定时间（足够让写线程进入等待状态），然后无条件释放锁。
  - 写线程通过测量 `AcquireRowLock` 的阻塞时间（`elapsed`）来验证“被阻塞过一段时间”这个事实。

这样：

- 写线程一定会在持有 `S` 的期间被阻塞（`elapsed >= 100ms`），验证锁冲突是有效的。
- 又能在读锁释放后顺利拿到 `X` 锁，整个测试不会产生死锁。

---

### 五、相关的另一个并发测试问题：快照测试用例逻辑错误

在调试并发测试的过程中，除了上面的死锁问题，还发现了 **`TestTransactionCommitSequence`** 的一个“测试逻辑错误”（不是 MVCC 实现的问题）：

- 原始测试做法：
  - 一次性对事务 1~10 调用 `OnTransactionCommitted(i)`，全部标记为已提交。
  - 然后在“事务 5”处调用 `BeginSnapshot(5)`，并断言：
    - 事务 3 对快照可见。
    - 事务 7 对快照不可见。
- 但当前 `MVCCManager::BeginSnapshot` 的实现是**返回已提交事务 ID 的最大值**作为快照 ID：
  - 因为 1~10 都被标记为已提交，快照值是 10。
  - 根据 `IsVisible` 规则，事务 7 自然是“已提交且 ID ≤ snapshot (10)” => 可见。
  - 导致测试断言失败，但实现逻辑本身是自洽的。

**修复方法：**

- 改为更符合语义的测试顺序：
  - 先只提交事务 1~4；
  - 调用 `BeginSnapshot(5)` 获取快照（此时最大已提交事务 ID 应为 4）；
  - 再依次提交 5~10；
  - 断言：
    - 事务 3（小于等于 4）在快照中可见；
    - 事务 7（大于 4）对快照不可见。

修复之后，所有详细并发测试（包括锁、MVCC、快照）均能通过。

---

### 六、小结与教训

1. **死锁有时候来自测试本身，而不是生产代码**
   - 本例中 `LockManager` 的实现是合理的，死锁是测试对“写事务完成状态”的错误依赖导致的。

2. **编写并发测试时要避免跨线程的循环依赖**
   - 尽量通过“时间窗口 + 行为观察”（比如测量阻塞时间）来判断是否发生了预期的阻塞，而不是让线程互相等待对方的状态变量。

3. **测试需要与实现模型保持一致**
   - 对于 `MVCCManager::BeginSnapshot` 这种简化实现，测试的期望也要基于“快照 == 当前最大已提交 TxId”的语义来构造。

这个死锁调试过程非常典型，既帮我们验证了锁管理器和 MVCC 的正确性，也提醒我们：**并发 bug 经常隐藏在“测试代码”和“假设”里，而不仅仅是业务代码里。**


