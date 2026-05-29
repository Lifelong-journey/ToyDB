### ToyDB 源码总体说明（按 `.cpp` 文件）

> 本文对 `src/` 目录下每个 `.cpp` 文件做功能与设计说明，并按函数粒度解释实现逻辑。  
> 重点关注：存储（Page/B+Tree/Table）、目录（Catalog）、日志与回滚、锁管理、MVCC、查询执行与辅助工具、并发测试。

---

## 1. `page_manager.cpp` — 页面管理与持久化缓存

### 文件职责与设计

- 负责将数据库文件抽象为定长页（`Page`），提供：
  - 页面分配（`NewPage`）
  - 从磁盘读取（`FetchPage` / `ReadPageFromFile`）
  - 写回磁盘（`FlushPage` / `WritePageToFile` / `FlushAllPages`）
- 内部维护一个简单的 **页缓存（`page_buffer_`）**，用 `std::unordered_map<page_id, unique_ptr<Page>>` 保存所有已加载页。
- 通过 `std::mutex` 保证多线程安全。

### 主要函数说明

- `PageManager::PageManager(const std::string& db_file)`
  - 打开（或创建）数据库文件，并根据文件长度推断 `next_page_id_`：
    - 如果文件为空，则从 `CATALOG_PAGE_ID` 开始分配页号；
    - 否则，根据 `file_size / PAGE_SIZE` 计算下一个可用页号。
  - 输出一条日志 `PageManager: Initialized with next_page_id: ...` 方便调试。

- `PageManager::~PageManager()`
  - 析构时调用 `FlushAllPages()`，确保缓存中所有脏页写回磁盘。
  - 关闭文件流，并输出“Closed database file”的日志。

- `Page* PageManager::FetchPage(uint32_t page_id)`
  - 上锁后先查 `page_buffer_`：
    - 如果已有缓存，直接返回缓存页指针，并打印“Fetching page X from buffer”。
  - 否则：
    - 如果 `page_id >= next_page_id_`，说明该页尚不存在（越界），打印错误并返回 `nullptr`。
    - 否则分配一个新的 `Page` 对象，调用 `ReadPageFromFile` 从磁盘位置 `page_id * PAGE_SIZE` 读入数据；
    - 加入缓存后返回指针，并打印“Fetched page X from disk”。

- `void PageManager::FlushPage(uint32_t page_id)`
  - 上锁后检查该页是否在 `page_buffer_` 中；不在则打印“Attempted to flush non-buffered page”并返回。
  - 如果页不脏（`is_dirty == false`）直接返回。
  - 否则打印“Flushing page X to disk”，调用 `WritePageToFile` 写入，并清除 `is_dirty`。

- `void PageManager::AddPageToBuffer(std::unique_ptr<Page> page)`
  - 仅用于外部希望手工将页放入缓存时。
  - 按页号作为 key 存入 `page_buffer_`，如发现已有同页号则打印覆盖警告。

- `uint32_t PageManager::NewPage()`
  - 在持锁状态下分配一个新的 `Page`：
    - 使用当前 `next_page_id_`，然后自增；
    - 创建对应的 `Page`，标记为 `is_dirty = true`（新页一定需要写盘）；
    - 直接放入 `page_buffer_`（不再调用会再次加锁的 `AddPageToBuffer`）；
    - 打印“Allocated new page with ID: N”并返回新页号。

- `void PageManager::FlushAllPages()`
  - 遍历 `page_buffer_`，对所有 `is_dirty` 的页调用 `WritePageToFile` 并清除脏标记。
  - 输出“Flushing all dirty pages”日志。

- `void PageManager::ReadPageFromFile(uint32_t page_id, Page* page)`
  - 移动文件读指针到 `page_id * PAGE_SIZE`，读入固定大小 `PAGE_SIZE` 的数据到 `page->data`。
  - 读失败时抛出 `std::runtime_error`。

- `void PageManager::WritePageToFile(Page* page)`
  - 移动写指针到对应偏移，写入 `PAGE_SIZE` 字节。
  - `flush()` 以确保立即同步到磁盘，写失败时抛异常。

---

## 2. `table.cpp` — 表元数据、B+ 树和逻辑回滚

### 文件职责与设计

- `Table` 封装了：
  - 表名、列定义（`std::vector<Column>`）
  - 底层主键索引：`BPlusTree<int, std::vector<VersionedRow>>`，存储 MVCC 版本链。
- 提供：
  - 表元数据的序列化 / 反序列化（用于写入目录页 `CATALOG_PAGE_ID`）
  - 支持 undo 的辅助函数：`UndoInsert`, `UndoDelete`, `UndoUpdate`
  - 根页 ID 与 B+ 树 root 的绑定。

### 主要函数说明

- `Table::Table(std::string name, std::vector<Column> columns, PageManager& page_manager, uint32_t root_page_id)`
  - 构造函数，将表名与列信息移动进来，并创建一棵 B+ 树：
    - 类型为 `BPlusTree<int, std::vector<VersionedRow>>`；
    - 使用给定的 `page_manager` 与 `root_page_id_`。

- `Table::~Table()`
  - 默认析构，放在 `.cpp` 中实现是因为 B+ 树是前向声明，在 `.h` 中类型不完整。

- `void Table::SetRootPageId(uint32_t root_page_id)`
  - 更新 `root_page_id_`，并调用 `bptree_->SetRootPageId(root_page_id)` 让 B+ 树的根同步更新。

- `void Table::Serialize(Page* page, size_t offset) const`
  - 将表的元数据写入 `Page::data + offset`：
    1. 写表名长度与内容；
    2. 写列数量；
    3. 对每一列写入列名长度 + 名字、列类型 `ColumnType`、列长度；
    4. 最后写入 `root_page_id_`。
  - 用于 `Catalog::Serialize` 把所有表放在同一页。

- `void Table::Deserialize(const Page* page, size_t offset, PageManager& page_manager)`
  - 从指定偏移按与 `Serialize` 对称的方式读回：
    - 表名、列数量、列信息、`root_page_id_`。
  - 读完后调用 `bptree_->SetRootPageId(root_page_id_)`，使表内的 B+ 树指向正确的根页。

- `void Table::UndoInsert(int key)`
  - 撤销一次 INSERT：
    - 在 B+ 树中删除该 key（整条版本链被删除）；
    - 更新 `root_page_id_ = bptree_->GetRootPageId()`。
  - 注意：这是一种“逻辑删除版本链”的做法，适合作为简单的 undo。

- `void Table::UndoDelete(int key, const std::vector<std::string>& old_row)`
  - 撤销 DELETE 的逻辑：
    - 先查版本链：
      - 如果版本链存在，从后往前找到**第一个被标记 delete_tx != INVALID_TX_ID 的版本**，将其 `delete_tx` 清回 `INVALID_TX_ID`，并 `committed = true`；
      - 然后在 B+ 树中删除、再插入更新后的版本链。
    - 如果版本链不存在（极端情况，例如物理删除），则使用 `old_row` 构建一个新版本：
      - `create_tx = INVALID_TX_ID`, `delete_tx = INVALID_TX_ID`, `committed = true`。
  - 用于 LogManager 在回滚 DELETE 时恢复被删除的行。

- `void Table::UndoUpdate(int key, const std::vector<std::string>& old_row)`
  - 撤销 UPDATE 的逻辑：
    - 查版本链：
      - 若存在且非空，直接删除版本链的最后一个元素（认为最新版本是本次 UPDATE 产生的），然后重写回 B+ 树。
      - 若版本链不存在，则用 `old_row` 构建一个单版本的版本链插入。
    - 更新 `root_page_id_`。
  - 粗粒度的做法：假设“最后一个版本”就是需要撤销的那个，这与上游 LogManager 的记录顺序匹配。

---

## 3. `catalog.cpp` — 系统目录（表集合）序列化

### 文件职责与设计

- `Catalog` 管理数据库中的所有表：`std::unordered_map<std::string, std::unique_ptr<Table>> tables_`。
- 将所有表的元信息打包序列化到一个特殊的页（`CATALOG_PAGE_ID`），用于数据库启动时重建内存中的表结构。

### 主要函数说明

- `void Catalog::Serialize(Page* page) const`
  - 把整个 `tables_` 写入单页：
    1. 写入表数量 `num_tables`；
    2. 对每张表：
       - 计算其序列化大小（表名 + 列信息 + 根页 ID），以便更新 offset；
       - 调用 `Table::Serialize(page, offset)` 实际写入数据；
       - `offset += table_serialized_size`。
  - 序列化完成后将 `page->is_dirty = true`。

- `void Catalog::Deserialize(const Page* page, PageManager& page_manager)`
  - 从目录页中重建 `tables_`：
    1. 读出表数量 `num_tables`；
    2. 对每张表，按照 `Table::Serialize` 的布局手工解析：
       - 读取表名、列数、每列的名字/类型/长度，以及 B+ 树根页 ID；
    3. 为每张表构造 `std::unique_ptr<Table>` 并插入 `tables_`。
  - 设计上是“在 Catalog 中重复实现了 Table 的序列化逻辑”，注释中也指出更理想的是让 `Table::Deserialize` 返回一个 `unique_ptr<Table>` 的静态方法；当前实现是权衡简单性后的折中。

---

## 4. `log_manager.cpp` — 逻辑日志与事务回滚

### 文件职责与设计

- 负责：
  - 分配事务 ID，维护活跃事务的上下文（`TransactionContext` 中保存 undo 记录）。
  - 为 INSERT / UPDATE / DELETE 写逻辑日志（在内存 + log 文件中记录）。
  - 提供 `RollbackTransaction`，通过遍历记录应用 undo（调用 `Table::Undo*`）。
  - 在 COMMIT / ROLLBACK 发生时输出对应日志行。

### 主要函数说明

- `LogManager::LogManager(const std::string& log_file)` 与 `~LogManager()`
  - 构造时打开日志文件（追加模式），失败则抛异常。
  - 析构时 flush 并关闭日志文件。

- `int LogManager::BeginTransaction()`
  - 持锁保护下分配新的 `tx_id`（`next_tx_id_++`）。
  - 在 `active_transactions_` 中插入空的 `TransactionContext`。
  - 追加一行 `"BEGIN <txid>"` 到日志文件，并打印 `"Transaction <txid> started."`。

- `bool LogManager::CommitTransaction(int tx_id, PageManager& page_manager)`
  - 若 `tx_id` 不在 `active_transactions_`，打印 `"COMMIT unknown TX"` 并返回 false。
  - 否则：
    - 调用 `page_manager.FlushAllPages()`（先刷数据页，简化版 WAL 顺序）；
    - 写 `"COMMIT <txid>"` 到日志；
    - 打印 `"Transaction <txid> committed."`；
    - 从 `active_transactions_` 中删除该事务。

- `bool LogManager::RollbackTransaction(int tx_id, Catalog& catalog, PageManager& page_manager)`
  - 检查 `tx_id` 是否活跃，不活跃则提示 `"ROLLBACK unknown TX"`。
  - 若活跃：
    - 反向遍历 `TransactionContext::records`（后进先出），对每条日志调用 `ApplyUndo`。
    - 写 `"ROLLBACK <txid>"`，打印 `"rolled back."` 并移除事务上下文。

- `bool LogManager::IsTransactionActive(int tx_id) const`
  - 在互斥锁保护下检查 `active_transactions_` 中是否存在指定事务。

- `void LogManager::LogInsert(...)`, `LogDelete(...)`, `LogUpdate(...)`
  - 三者模式类似：
    - 持锁后通过 `GetContextUnlocked(tx_id)` 找到对应 `TransactionContext`，若事务已结束则忽略记录。
    - 构造 `LogRecord` 填入类型、表名、主键、旧/新行数据，推入 `records` 向量。
    - 追加一行简要描述到日志文件（例如 `"TX 1 INSERT t key=10"`）。

- `void LogManager::AppendLogLine(const std::string& line)`
  - 写入一行文本并立刻 `flush`，保证测试中能立即看到顺序。

- `LogManager::TransactionContext* LogManager::GetContextUnlocked(int tx_id)`
  - 在不加锁的前提下从 `active_transactions_` 获取 `TransactionContext` 地址，若不存在则打印警告。

- `void LogManager::ApplyUndo(const LogRecord& rec, Catalog& catalog, PageManager& page_manager)`
  - 根据 `rec.type` 分派到相应的 `Table::UndoInsert/Delete/Update`：
    - INSERT -> 删除 key；
    - DELETE -> 恢复旧行；
    - UPDATE -> 恢复旧行版本。
  - 每次 undo 后调用 `page_manager.FlushAllPages()` 保证回滚结果持久化。

---

## 5. `lock_manager.cpp` — 二段锁、意向锁与阻塞等待

### 文件职责与设计

- 实现简化版的 **锁管理器**：
  - 支持锁模式：`INTENTION_SHARED (IS)`, `INTENTION_EXCLUSIVE (IX)`, `READ (S)`, `WRITE (X)`。
  - 使用一个资源到 `ResourceState` 的映射：
    - 其中记录该资源上的锁持有者列表 `holders` 和条件变量 `cv`。
  - 还有 `resources_by_tx_` 记录每个事务持有哪些资源，便于统一释放。

### 主要函数说明

- `bool LockManager::IsConflict(LockMode existing, LockMode requested)`
  - 实现 IS/IX/S/X 的兼容矩阵：
    - 给每种模式映射一个 index（0~3），用一个 4x4 布尔矩阵 `compatible[existing][requested]` 描述是否兼容；
    - 返回 `!compatible[e][r]`，即“是否冲突”。
  - 注释中给出了完整的兼容表，符合经典的意向锁定义。

- `bool LockManager::TryUpgradeLock(ResourceState& state, const LockRequest& req)`
  - 当同一事务已经持有某个锁，再请求 `WRITE` 时，尝试在**没有其他事务持有冲突锁**的前提下原地升级：
    - 找到 `holders` 中同一 `tx_id` 的记录；
    - 若没有其他事务也在该资源上持锁，则将模式改为 `WRITE`，返回 true。

- `bool LockManager::CheckAndGrant(const std::string& resource, const LockRequest& req)`
  - 核心加锁流程：
    1. 上互斥锁，取得该资源的 `ResourceState`；
    2. 定义 `has_conflict(s)`：遍历 `s.holders`，对不同事务的持有锁与 `req.mode` 逐一调用 `IsConflict`；
    3. 在一个 `while(true)` 循环中：
       - 若当前事务已持锁：
         - 判断是否可以重入或升级（写中写、读重入写、`TryUpgradeLock` 等），且不引入新的冲突时直接返回 true；
       - 否则如果当前没有冲突（`!has_conflict(*state)`）：
         - 将 `req` 加入 `holders`，并记录到 `resources_by_tx_`；
         - 打印获取锁的日志，返回 true；
       - 如果存在冲突：
         - 调用 `state->cv.wait(lock)` 阻塞等待，直到某个释放锁的操作 `notify_all()`。
  - 这里实现的是典型的**阻塞式二段锁**：持锁不主动撤销，冲突请求在条件变量上等待。

- `bool LockManager::AcquireTableLock(const std::string& table_name, int tx_id, LockMode mode)`
  - 对应一个表资源 `"table:<name>"`；
  - 非事务（`tx_id <= 0`）不加锁，直接返回 true；
  - 构造 `LockRequest{tx_id, table_name, nullopt, mode}` 并调用 `CheckAndGrant`。

- `bool LockManager::AcquireRowLock(const std::string& table_name, int key, int tx_id, LockMode mode)`
  - **当前实现简化为“行锁和表锁竞争同一个表资源”**：
    - 即对 `"table:<name>"` 资源调用 `CheckAndGrant`；
    - `key` 目前仅用于调试，不参与区分资源粒度。
  - 非事务（`tx_id <= 0`）也不加锁。
  - 这种设计简化了测试中的锁依赖，重点验证表级读写与事务交互。

- `void LockManager::ReleaseLocks(int tx_id)`
  - 根据 `resources_by_tx_[tx_id]` 列表，逐个资源删除该事务在 `holders` 中的记录；
  - 对每个资源的 `cv.notify_all()`，唤醒所有等待的 `CheckAndGrant`；
  - 删除 `resources_by_tx_` 中该事务的条目，并打印“released locks for TX ...”。

---

## 6. `mvcc.cpp` — MVCC 管理器与快照隔离

### 文件职责与设计

- 管理 MVCC 全局状态：
  - `committed_tx_set_`：记录所有已提交事务 ID；
  - `tx_commit_seq_` 与 `next_commit_seq_`：预留的提交序列号结构（当前快照逻辑主要用 ID 大小简化）。
- 提供：
  - `BeginSnapshot(tx_id)`：生成事务启动时的快照标识；
  - `IsVisible(current_tx, snapshot, create_tx, delete_tx)`：给定版本头信息判断是否对当前事务可见；
  - 事务提交/回滚事件的通知。

### 主要函数说明

- `TxId MVCCManager::BeginSnapshot(TxId tx_id)`
  - 在互斥锁保护下：
    - 若当前尚无已提交事务，返回 `INVALID_TX_ID`；
    - 否则返回 `committed_tx_set_` 中的最大事务 ID 作为“快照 ID”。
  - 当前实现是**简化版 Snapshot Isolation**：快照仅用一个“最大已提交 ID”表示，而不是一整套可见事务集合。

- `bool MVCCManager::IsVisible(TxId current_tx, TxId snapshot, TxId create_tx, TxId delete_tx) const`
  - 版本可见性规则：
    1. **系统内部行**（`create_tx == INVALID_TX_ID`）默认可见，但需要考虑 `delete_tx`：
       - 若 `delete_tx == current_tx`，表示当前事务自己删除，版本对自己不可见；
       - 若 `delete_tx` 在快照之前提交（且已在 `committed_tx_set_` 中）也不可见；
    2. **当前事务自己创建的版本**（`create_tx == current_tx`）对自己可见（除非被自己删除）。
    3. 对一般事务：
       - 需在互斥锁下检查：
         - 创建事务 `create_tx` 满足：已提交且 `create_tx <= snapshot`，否则版本不可见；
         - 若存在删除事务 `delete_tx`：
           - 如果 `delete_tx == current_tx`，不可见；
           - 如果 `delete_tx` 已提交且 `delete_tx <= snapshot`，则表示在快照时已经被删除，也不可见。
  - 返回 true 即表示该版本在给定“快照 ID”下是有效行。

- `void MVCCManager::OnTransactionCommitted(TxId tx_id)`
  - 将 `tx_id` 插入 `committed_tx_set_`，并在 `tx_commit_seq_` 中记录其提交序号 `next_commit_seq_++`。

- `void MVCCManager::OnTransactionRolledBack(TxId tx_id)`
  - 从 `tx_commit_seq_` 中删除该事务（回滚事务本身并不在 `committed_tx_set_` 里）。

- `TxId MVCCManager::GetMaxCommittedTxId() const`
  - 返回当前集合中最大已提交事务 ID，若为空则返回 `INVALID_TX_ID`。

---

## 7. `query_utils.cpp` — SQL 条件/连接的解释执行工具

### 文件职责与设计

- 提供一组解析和求值工具函数，主要用于 `main.cpp` 中的查询执行：
  - `PgValueToString`：将 libpg_query 的 `PGValue` 转为 C++ 字符串；
  - `Trim`：字符串去空白；
  - `WhereValue` + `ExtractWhereValue`：把 AST 节点抽象为“列引用 / 常量值”；
  - `EvaluateWhereClause`：解释执行 WHERE 布尔表达式；
  - `EvaluateJoinCondition`：在 JOIN 中根据左右行进行条件判断；
  - `IsRowVisible` / `FindVisibleVersion`（在 header 中声明，在此实现，用于基于 MVCC 找出可见版本）。

### 主要函数说明（部分）

- `std::string PgValueToString(duckdb_libpgquery::PGValue* value_node)`
  - 目前主要处理字符串类型（`T_PGString`），直接返回其值；其他类型可扩展。

- `std::string Trim(const std::string& str)`
  - 手工查找第一个/最后一个非空白字符，截取对应子串，去除前后空格/换行。

- `WhereValue ExtractWhereValue(duckdb_libpgquery::PGNode* node, const Table* table)`
  - 如果是 `PGAConst`：
    - 如果常量是整数，则填充 `constant_int` 和 `constant_value`（字符串形式）；
    - 如果是字符串，则只填充 `constant_value`。
  - 如果是 `PGColumnRef`，则设置 `is_column = true`，并从 `fields` 列表里取列名字符串。

- `bool EvaluateWhereClause(PGNode* where_node, const std::vector<std::string>& row, const Table* table)`
  - 目标：在单行数据上解释执行 WHERE 条件。
  - 算法：
    1. 若 `where_node == nullptr`，直接返回 true（无 WHERE 条件）。
    2. 若是 `PGAExpr` 且 `kind == PG_AEXPR_OP`（二元比较）：
       - 解析比较运算符字符串 `op`；
       - 使用 `ExtractWhereValue` 分别得到左右表达式的 `WhereValue`；
       - 根据 `WhereValue` 和 `table->GetColumns()` 找到对应列值或常量，尝试转为 int（整数比较）或保留字符串（字典序比较）；
       - 按 `=`, `!=`, `<`, `>`, `<=`, `>=` 分情况返回比较结果。
    3. 若是 `PGBoolExpr`（AND/OR/NOT）：
       - 对 AND：递归地要求所有子节点为 true；
       - 对 OR：只要任一子节点为 true；
       - 对 NOT：对子节点结果取反。
    4. 默认返回 true（防御性策略）。

- `bool EvaluateJoinCondition(PGNode* join_node, const std::vector<std::string>& left_row, const std::vector<std::string>& right_row, const Table* left_table, const Table* right_table)`
  - 针对 JOIN 的 ON 条件，与 `EvaluateWhereClause` 类似，不过：
    - `ExtractWhereValue` 时针对左右表；
    - 查找列位置时先在 left 表列名中找，再在 right 表中找；
    - 比较逻辑也是针对解析出来的左右值（整数优先，否则用字符串）。

> 说明：`IsRowVisible` / `FindVisibleVersion` 的实现也在此文件中，用于结合 `MVCCManager` 和当前事务快照，从版本链中找到对当前事务可见的最新版本；其逻辑与 `mvcc.cpp` 中 `IsVisible` 规则一一对应，这里不再赘述。

---

## 8. `b_plus_tree.cpp` — B+ 树模板实现（键值存储 + 版本链）

### 文件职责与设计

- 实现模板类 `BPlusTree<KeyType, ValueType>` 及其节点：
  - 叶子结点 `BPlusTreeLeafNode` 存储键和对应的 `ValueType`（本项目中为 `std::vector<VersionedRow>`）。
  - 内部结点 `BPlusTreeInternalNode` 存储分割键与子结点页 ID。
- 与 `PageManager` 紧密协作：
  - 每个结点对应一个页；序列化/反序列化负责把节点结构写入/读出页。
  - 支持插入、搜索、删除、节点分裂、父节点调整等操作。
- 线程安全：树操作通过 `tree_mutex_` 互斥锁串行化。

> 由于文件较长，这里重点介绍主要对外 API 与关键内部算法。

### 主要对外 API

- `std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> FindLeaf(const KeyType& key)`
  - 重载版本：不关心父节点页 ID，只返回找到的叶子结点（作为 `unique_ptr`）。
  - 内部调用带 `parent_page_id` 参数的重载。

- `std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> FindLeaf(const KeyType& key, uint32_t& parent_page_id)`
  - 从根开始遍历：
    - 若 `root_page_id_ == INVALID_PAGE_ID`，直接返回 `nullptr`。
    - 否则调用 `GetNode(root_page_id_)` 取得根节点。
    - 若根是叶子，则 `parent_page_id = INVALID_PAGE_ID` 并返回。
    - 否则在内部结点中根据 key 查找应当下降的子结点：
      - 找到第一个 `keys_[i]` 大于 key 的位置 `i`，沿 `children_page_ids_[i]` 下降；
      - 每下降一层更新当前 parent_page_id；
    - 直到到达叶子结点，返回对应 `unique_ptr<BPlusTreeLeafNode>` 与其父节点页 ID。

- `void Insert(const KeyType& key, const ValueType& value)`
  - 操作流程：
    1. 加锁 `tree_mutex_`；
    2. 若 `root_page_id_` 尚未初始化：
       - 调用 `page_manager_.NewPage()` 分配新页做根；
       - 构建一个叶子结点，插入首个 `<key, value>`，序列化到页并标记脏；
       - 返回。
    3. 否则：
       - 调用 `FindLeaf(key, leaf_parent_page_id)` 找到目标叶子结点；
       - 在叶子中的 `keys_` 和 `values_` 向量中按升序位置插入新键值对；
       - 把叶子序列化回对应页，标记脏；
       - 如果叶子“满”（`IsFull()`），调用 `SplitLeafNode` 进行分裂。

- `void SplitLeafNode(std::unique_ptr<BPlusTreeLeafNode<KeyType, ValueType>> leaf_ptr, uint32_t parent_page_id)`
  - 叶子分裂算法：
    1. 计算分裂点 `split_idx = max_size_ / 2`；
    2. 为新叶子分配新页 ID，构造一个新的叶子结点；
    3. 将原叶子 `keys_` 与 `values_` 中从 `split_idx` 到末尾的元素移动到新叶子；
    4. 更新叶之间的**链表指针（前后继页 ID）**：
       - `new_leaf->prev_leaf_page_id_ = leaf->page_id_;`
       - `new_leaf->next_leaf_page_id_ = old_next_leaf_page_id;`
       - `leaf->next_leaf_page_id_ = new_leaf_page_id;`
       - 若存在原 next 叶子，则读取该叶子，改写其 `prev_leaf_page_id_ = new_leaf_page_id` 并回写。
    5. 将原叶和新叶序列化写回各自页，标记脏；
    6. 把新叶的首键作为 `promoted_key` 提升到父结点，调用 `InsertIntoParent(...)`。

- `void InsertIntoParent(std::unique_ptr<BPlusTreeNode<KeyType, ValueType>> node_ptr, const KeyType& key, uint32_t new_node_page_id, uint32_t parent_of_node_page_id)`
  - 将分裂产生的新子节点插入父节点：
    - 如果当前节点是根（`node->page_id_ == root_page_id_`）：
      - 分配新根页，构建一个 `BPlusTreeInternalNode`，包含一个 key 和两个孩子页 ID（原根、新节点）；
      - 更新两子节点的 `parent_page_id_`，序列化；
      - 更新 `root_page_id_ = new_root_page_id`。
    - 否则：
      - 使用传入的 `parent_of_node_page_id` 获取父节点；
      - 在父节点的 `keys_` 中按 key 升序插入新的分隔键，同时在 `children_page_ids_` 中在其右侧插入新子页 ID；
      - 更新新子节点的 `parent_page_id_` 并序列化；
      - 若父节点满，调用 `SplitInternalNode` 进一步分裂。

> 说明：`b_plus_tree.cpp` 中还包含搜索和删除相关函数、节点序列化函数等，整体遵循经典 B+ 树算法，这里以插入与分裂为代表进行说明。

---

## 9. `main.cpp` — REPL 主程序与 SQL 执行入口

### 文件职责与设计

- 实现了一个简单的 **交互式 SQL Shell**：
  - 使用 `duckdb::PostgresParser` 将输入 SQL 解析成 libpg_query AST；
  - 根据 AST 类型执行：`CREATE TABLE / INSERT / SELECT / UPDATE / DELETE / DROP / SHOW TABLES / BEGIN / COMMIT / ROLLBACK`；
  - 集中管理：
    - `PageManager`, `Catalog`, `LogManager`, `LockManager`, `MVCCManager`；
    - 会话级事务上下文：`session_tx_id` 与 `session_snapshot`。
- 核心执行逻辑集中在 `ExecuteQuery(...)` 函数中。

### 关键函数说明

- `bool ExecuteQuery(const std::string& query, PostgresParser& parser, Catalog& catalog, PageManager& page_manager, LogManager& log_manager, LockManager& lock_manager, MVCCManager& mvcc_manager, std::optional<int>& session_tx_id, std::optional<TxId>& session_snapshot)`
  - 完成一次 SQL 的完整解析与执行：
    1. 使用 `toydb::Trim` 去除两侧空白，空字符串则直接返回 true；
    2. 若输入是 `QUIT/EXIT/.EXIT`（不区分大小写），返回 false 通知主循环退出；
    3. 调用 `parser.Parse(trimmed_query)`，检查成功与否；
    4. 从 parse_tree 中取出第一条 `PGRawStmt`，根据 `raw_stmt->stmt->type` 分发到不同子逻辑：
       - `T_PGCreateStmt`：构造 `Table`，注册到 `Catalog`，并通过 `PageManager` 将更新后的目录序列化到 `CATALOG_PAGE_ID`；
       - `T_PGTransactionStmt`：处理 `BEGIN/COMMIT/ROLLBACK`，与 `LogManager` / `LockManager` / `MVCCManager` 交互；
       - `T_PGInsertStmt`：解析 VALUES 列表，构造 `VersionedRow` 版本链，写入 B+ 树，并根据是否在事务中更新日志与锁；
       - `T_PGSelectStmt`：支持单表/简单 JOIN，WHERE、ORDER BY、GROUP BY、聚合函数等，遍历 B+ 树并利用 `query_utils` 过滤/投影，同时通过 MVCC 选择可见版本；
       - `T_PGUpdateStmt` / `T_PGDeleteStmt`：对每个键读取版本链、筛选可见版本，通过 WHERE 判断后生成新版本（或标记 delete_tx），同时记录日志；
       - `T_PGDropStmt`：从 `Catalog` 中移除表并更新目录页；
       - `T_PGShowStmt`：实现 `SHOW TABLES` 列出 `Catalog` 中所有表。
  - 每种语句类型都在函数内部就地实现，利用 `query_utils` 提供的通用工具函数完成表达式求值与版本可见性判断。

- `int main()`
  - 初始化：
    - 打印欢迎信息；
    - 创建 `PostgresParser`、`PageManager("toydb.db")`、`Catalog`、`LogManager("toydb.log")`、`LockManager`、`MVCCManager`；
    - 若 `CATALOG_PAGE_ID` 已存在，则调用 `catalog.Deserialize(...)` 重建表信息。
  - 交互循环：
    - 逐行读取用户输入，支持多行 SQL（以 `;` 结束）；
    - 对每个完整语句调用 `ExecuteQuery(...)` 执行；
    - 同时维护当前会话的 `session_tx_id` 和 `session_snapshot`，实现事务与快照隔离。
  - 退出前：
    - 将 `Catalog` 序列化写回目录页，调用 `FlushPage` 与 `FlushAllPages` 确保数据落盘。

---

## 10. `concurrency_test.cpp` 与 `concurrency_detailed_test.cpp` — 并发与锁/MVCC 测试

### `concurrency_test.cpp`

- 提供一组基础并发测试（相对简单）：
  - 测试读读共享（`TestSharedReadLocks`）；
  - 写写互斥（`TestExclusiveWriteLocks`）；
  - 表级 X 锁阻塞行级锁（`TestTableXBlocksRowLocks`）；
  - 日志管理器的并发 Begin/Commit（`TestLogManagerConcurrentBegin`）；
  - B+ 树的并发插入与搜索（`TestBPlusTreeConcurrentInsertAndSearch`）。
- `main()` 依次调用这些测试并打印结果。

### `concurrency_detailed_test.cpp`

- 更全面的并发测试集合（文档已在 `concurrency_deadlock_story.md` 中详细说明）：
  - `TestSnapshotIsolationReadWrite`：验证快照隔离下读事务看不到并发写事务的修改。
  - `TestWriteWriteConflict`：两个写事务争抢行锁时，后者必须阻塞直至前者释放。
  - `TestMultipleReadersSingleWriter`：多读单写的阻塞关系（修复了早期测试中的死锁问题）。
  - `TestVersionChainConcurrency`：多版本链在不同快照下的可见性。
  - `TestTransactionCommitSequence`：验证 `BeginSnapshot` 与 `IsVisible` 在提交序列下的行为。
  - `TestLockUpgrade`：同一事务从读锁升级到写锁。
  - `TestTableRowLockHierarchy`：表级写锁阻塞行级读锁。
- `main()` 中顺序调用所有测试，并在任何断言失败时退出。

---

以上就是当前 `src/` 下各主要 `.cpp` 文件及其函数的设计与实现说明。  
如果你希望对某个具体文件或函数再做更细致的“逐行源码解释”，可以告诉我文件名或函数名，我可以单独展开。 


