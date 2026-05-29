### ToyDB 项目总体总结

> 本文从“产品功能”和“内部实现能力”两个角度，对当前版本的 ToyDB 做一个整体总结：  
> 哪些已经实现，哪些只做了简化版本，哪些暂未覆盖。

---

## 1. 总体定位

ToyDB 是一个教学/实验性质的 **单机关系型数据库原型**，重点在于：

- **存储引擎**：页管理 + B+ 树主键索引 + 表/目录元数据持久化；
- **事务与并发控制**：两阶段锁（2PL）+ MVCC 快照隔离 + 逻辑日志和回滚；
- **SQL 前端**：基于 DuckDB 的 PostgreSQL 语法解析，支持一部分常见 SQL 子集；
- **测试与文档**：完善的 SQL 回归测试和多线程并发测试，以及设计/实现过程文档。

它不是一个生产级数据库，而是为“理解数据库内核原理”提供的一个可运行、可调试的小型实现。

---

## 2. 已实现的主要功能

### 2.1 存储与索引

- **页管理 (`PageManager`)**
  - 固定大小页（`PAGE_SIZE`）的读写与缓存：
    - 支持 `FetchPage` / `NewPage` / `FlushPage` / `FlushAllPages`。
  - 所有数据（目录、B+ 树节点、表元数据）都映射到单一数据文件 `toydb.db` 中。
  - 多线程安全（内部使用 `std::mutex`）。

- **B+ 树主键索引 (`BPlusTree<int, std::vector<VersionedRow>>`)**
  - 索引 Key：整型主键 `int`（当前假设第一列为主键）。
  - 索引 Value：**MVCC 版本链**（一个 key 对应多个 `VersionedRow`）。
  - 功能：
    - `Insert(key, value)`：插入新版本（必要时分裂叶子与内部节点）。
    - `Search(key, value)`：按主键查找版本链。
    - `Delete(key)`：删除某个键及其版本链（用于 undo 等场景）。
    - `GetAllValues()`：顺序扫描叶子链表，返回所有 `<key, value>`，用于全表扫描与 JOIN。
  - 节点以页为单位序列化到磁盘，支持树高动态增长和收缩。

- **表 (`Table`) 与目录 (`Catalog`)**
  - `Table`：
    - 保存表名、列定义（`name/type/length`）、B+ 树根页 ID；
    - 提供 `Serialize/Deserialize` 用于把元数据写入/读取目录页；
    - 提供 `UndoInsert/UndoDelete/UndoUpdate`，配合日志实现逻辑回滚。
  - `Catalog`：
    - 管理所有表的集合 `unordered_map<string, unique_ptr<Table>>`；
    - 将所有表的元信息打包存储在一个特殊页 `CATALOG_PAGE_ID`；
    - 启动时反序列化，关闭时序列化。

### 2.2 事务、日志与 MVCC

- **逻辑日志与回滚 (`LogManager`)**
  - 每个事务有一个 `TransactionContext`，记录其所有 DML 操作的逻辑日志：
    - `LogInsert(tx, table, key, new_row)`
    - `LogDelete(tx, table, key, old_row)`
    - `LogUpdate(tx, table, key, old_row, new_row)`
  - `BeginTransaction()` 分配递增的事务 ID，并写入 `BEGIN tx` 日志行。
  - `CommitTransaction(tx)`：
    - 先调用 `PageManager::FlushAllPages()` 刷盘；
    - 写 `COMMIT tx` 行，移除事务上下文。
  - `RollbackTransaction(tx)`：
    - 反向遍历 `TransactionContext.records` 调用 `ApplyUndo`：
      - INSERT -> `Table::UndoInsert`
      - DELETE -> `Table::UndoDelete`
      - UPDATE -> `Table::UndoUpdate`
    - 写 `ROLLBACK tx` 行，移除事务上下文。
  - 注意：实现的是 **逻辑 Undo + 简化 WAL 顺序**，可以正确回滚活动事务，但**不提供崩溃后的 REDO 恢复**。

- **锁管理 (`LockManager`)：2PL**
  - 支持四种锁模式：`IS` / `IX` / `S` / `X`，符合经典意向锁兼容矩阵。
  - 简化实现：行锁与表锁目前都映射到 `"table:<name>"` 资源上竞争，保证：
    - 表级 X 锁会阻塞任何行级 S/X 锁；
    - 行级之间的读读共享、读写/写写互斥。
  - 提供：
    - `AcquireTableLock(table, tx, mode)`
    - `AcquireRowLock(table, key, tx, mode)`
    - `ReleaseLocks(tx)`：释放某事务所有锁并 `notify_all` 唤醒等待者。
  - 阻塞模型：
    - 冲突请求在 `std::condition_variable::wait` 上等待；
    - 无死锁检测，依赖用户或测试避免循环等待（复杂死锁场景不处理）。

- **MVCC 管理 (`MVCCManager`)**
  - 维护：
    - `committed_tx_set_`：所有已提交事务 ID；
    - `tx_commit_seq_` 与 `next_commit_seq_`：预留提交序列号（当前快照逻辑仅用 max ID）。
  - `BeginSnapshot(tx)`：
    - 返回当前已提交事务 ID 的最大值，作为该事务的 **快照 ID**。
  - `IsVisible(current_tx, snapshot, create_tx, delete_tx)`：
    - 实现简化版 **快照隔离（Snapshot Isolation）** 规则：
      - 创建事务必须在快照之前已提交（`create_tx <= snapshot` 且在 `committed_tx_set_` 中）。
      - 删除事务若已在快照时刻提交，则该版本对快照不可见。
      - 当前事务自己创建的版本对自己可见（未提交也可见），自己删除的版本对自己不可见。
  - `OnTransactionCommitted(tx)` / `OnTransactionRolledBack(tx)`：
    - 更新提交集合与辅助映射。

- **整体隔离级别**
  - 使用 **2PL（锁管理） + MVCC 快照隔离** 一起工作：
    - 锁：控制写写/读写冲突，避免脏写等问题；
    - MVCC：保证读取的是某个一致快照，避免读取未提交数据。
  - 效果接近 **Snapshot Isolation**，不严格保证所有 Serializable 性能。

### 2.3 SQL 支持（解析与执行）

ToyDB 通过 DuckDB 的 Postgres 解析器支持一个子集的 SQL 功能，执行逻辑集中在 `main.cpp` 的 `ExecuteQuery` 中。

- **DDL**
  - `CREATE TABLE name (col type, ...)`
    - 支持 `int4` 和 `varchar(n)`，将列元信息注册到 `Catalog`，并持久化到目录页。
  - `DROP TABLE name`
    - 从 `Catalog` 删除表并更新目录页。

- **DML**
  - `INSERT INTO tbl VALUES (...)`（单行插入）
    - 假设第一列为整型主键。
    - 在事务中执行时：
      - 为行和表加写锁；
      - 记录 Insert 日志；
      - 新版本 `create_tx = 当前事务 ID`，`committed = false`。
    - 非事务执行时：版本视为立即提交。
  - `SELECT ... FROM tbl ...`
    - 支持：
      - 单表 SELECT；
      - 单个 INNER JOIN：`FROM A JOIN B ON condition`；
      - WHERE 条件（布尔表达式 AND/OR/NOT，比较运算 = / != / <> / > / < / >= / <=）；
      - 简单 ORDER BY 单列；
      - 聚合函数：`COUNT/SUM/AVG/MAX/MIN`，可带 WHERE 和单列 GROUP BY。
    - MVCC：通过 `FindVisibleVersion(版本链, session_tx_id, snapshot, mvcc_manager)` 为每行选择对当前事务可见的最新版本。
  - `UPDATE tbl SET ... WHERE ...`
    - 遍历所有 key 的版本链：
      - 找出当前事务可见版本；
      - WHERE 不匹配直接跳过；
      - 加行/表写锁；
      - 记录 Update 日志；
      - 将可见版本标记为删除（`delete_tx = 当前事务 ID`），并在版本链末尾追加新版本（新 row 数据，`create_tx = 当前事务 ID`）。
  - `DELETE FROM tbl WHERE ...`
    - 类似 UPDATE：
      - 对可见版本设置 `delete_tx = 当前事务 ID` 表示逻辑删除；
      - 记录 Delete 日志；
      - 不立即物理删除旧版本，为 MVCC 与回滚保留历史。

- **事务控制与会话管理**
  - 支持：
    - `BEGIN` / `START TRANSACTION`
    - `COMMIT`
    - `ROLLBACK`
  - 事务是 **会话级** 的：
    - 每个 `./toy_db` 交互会话有一个 `session_tx_id` 和 `session_snapshot`。
    - 同一会话内一次只能有一个活动事务。

- **其他命令**
  - `SHOW TABLES`：列出当前 Catalog 中所有表名。
  - 退出命令：
    - 在 SQL 里输入：`QUIT;` / `EXIT;` / `.EXIT;`
    - 或在交互模式下直接输入 `QUIT` / `EXIT` / `.EXIT`（无分号）立刻退出。

### 2.4 测试与工具

- **SQL 回归测试（`tests/`）**
  - `01_basic.sql`：基础 CRUD 与 SHOW TABLES。
  - `02_where_agg.sql`：WHERE 条件与聚合。
  - `03_join.sql`：JOIN 测试。
  - `04_order_group.sql`：ORDER BY 与 GROUP BY。
  - `05_txn_log.sql`：事务与回滚场景。
  - `06`~`12`：针对 MVCC、并发读写、事务隔离等扩展测试。
  - `run_regression_tests.sh` 会：
    - 先运行并发 C++ 测试；
    - 再依次运行所有 SQL 文件，将输出归一化后与 `.out` 基线比较。

- **并发测试（C++）**
  - `concurrency_test.cpp`：
    - 测试行级读读共享、写写互斥、表级 X 锁与行级锁冲突、B+ 树并发插入与搜索等基础场景。
  - `concurrency_detailed_test.cpp`：
    - 更细粒度测试锁/事务/MVCC 行为：
      - 读事务看不到并发写事务的修改；
      - 写写冲突阻塞；
      - 多读单写；
      - 版本链在不同快照下的可见性；
      - 提交序列与快照可见性；
      - 锁升级、表级/行级锁层次等。
  - 相关调试过程记录在 `docs/concurrency_deadlock_story.md`。

---

## 3. 尚未实现或仅简化实现的功能

### 3.1 存储与恢复局限

- **没有真正的 WAL/REDO 崩溃恢复**
  - 当前日志仅用于**活动事务回滚**：
    - 崩溃后重启不会根据日志做 REDO 或 UNDO，数据库文件处于“最近一次正常关闭时的状态”。
  - 没有 checkpoint、LSN 等机制。

- **页面缓存策略很简单**
  - 使用 `unordered_map<page_id, unique_ptr<Page>>` 作为缓存；
  - 不支持 LRU/LFU 替换，也不限制内存占用；
  - 适合作为教学示例，但不适合大规模数据集。

- **没有多文件/多表空间支持**
  - 所有数据都存在单一 `toydb.db` 文件中，目录页也是其中的一页。

### 3.2 SQL 与查询引擎的局限

- **SQL 语法子集有限**
  - 支持简单 SELECT/INSERT/UPDATE/DELETE/CREATE/DROP/SHOW/事务控制；
  - 不支持：
    - 多表/多级 JOIN（目前仅支持 1 个 JOIN）；
    - 子查询、CTE（WITH）、视图；
    - 索引 DDL（除了内建的主键 B+ 树）；
    - 复杂表达式（函数调用、CASE、算术表达式等）。

- **无查询优化器**
  - 执行计划是**固定的嵌套循环与全表扫描**：
    - WHERE 主键等值查询支持一些“利用 key 直接定位”的优化，但整体上仍是 naive 实现；
    - JOIN 采用简单嵌套循环，未做哈希/排序优化。

- **类型系统较简单**
  - 仅支持 `int4` 和 `varchar(n)` 两种类型；
  - 没有 NULL 语义（所有值均为非 NULL，解析层不处理 IS NULL/IS NOT NULL）。

### 3.3 事务与并发控制的局限

- **快照隔离是简化版**
  - 快照用“最大已提交事务 ID”来表示，没有完整的 committed set 向量或版本时间戳；
  - 在某些极端交错场景下，行为与真正的 PostgreSQL SI 可能不同（例如长事务与插队提交事务的复杂可见性）。

- **锁管理简化**
  - 行锁和表锁共享同一资源 `"table:<name>"`，即：
    - 实际上是**表级锁模型**，行级锁仅在接口层有所区分；
    - 不支持真正的高并发现行访问（会比真实数据库更保守）。
  - 没有死锁检测和超时机制：
    - 如果测试或用户 SQL 造成循环等待，线程会一直阻塞。

- **单机单进程**
  - 没有网络协议或多客户端连接支持；
  - 所有 SQL 都通过标准输入/输出进行交互。

### 3.4 其他未实现部分

- **权限/用户管理**：无身份验证/授权模型。
- **高级索引结构**：如多列索引、二级索引、哈希索引等。
- **统计信息与代价估计**：没有收集表统计数据，也没有成本模型。
- **备份/导入导出**：没有专门的数据导入/导出工具，仅支持通过 SQL 脚本导入。

---

## 4. 总结与后续可扩展方向

当前版本的 ToyDB，已经具备一个“小而全”的数据库内核基本面貌：

- 有 **持久化存储**（页管理 + B+ 树）；
- 有 **事务/日志/回滚**；
- 有 **2PL + MVCC 的并发控制**；
- 能用一个简化但足够表达力的 **SQL 子集** 完成常见教学场景；
- 附带一套完整的 SQL 回归测试与多线程并发测试，用于保护行为语义。

如果要进一步演进，可以考虑：

- 在日志层引入真正的 WAL + 崩溃恢复；
- 拆分表级/行级资源，支持更细粒度锁和死锁检测；
- 完善 MVCC 快照表示，支持更接近 PostgreSQL 的可见性规则；
- 实现简单的查询优化器（选择索引/Join 顺序/执行计划）；
- 引入网络协议（如简化版 Postgres wire 或自定义文本协议）以支持多客户端连接。

但就教学目的而言，ToyDB 目前已经提供了一个相当完整的“从 SQL 到磁盘”的闭环，可以帮助读者理解数据库内核中从解析、执行到并发与持久化的关键组件与算法。



