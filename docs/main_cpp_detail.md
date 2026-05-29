### `main.cpp` 详细说明（ToyDB 交互式 SQL 执行入口）

本文件实现 ToyDB 的命令行交互程序和 SQL 执行主逻辑，核心分为两部分：

- 会话级 REPL（`main()`）：负责初始化各个子系统，并循环读取用户输入。
- 查询执行函数（`ExecuteQuery`）：给定一条 SQL 字符串，使用 `PostgresParser` 解析出 AST，然后根据 AST 类型执行不同操作。

下文按函数和逻辑块详细解释源码行为。

---

## 1. 头文件与依赖

```cpp
#include <iostream>
#include <string>
#include <sstream>
#include <algorithm>
#include <limits>
#include <map>
#include "postgres_parser.hpp"
#include "b_plus_tree.h"
#include "table.h"
#include "catalog.h"
#include "page_manager.h"
#include "definitions.h"
#include "nodes/parsenodes.hpp"
#include <cstring>
#include "nodes/pg_list.hpp"
#include "log_manager.h"
#include "lock_manager.h"
#include "mvcc.h"
#include "query_utils.h"
#include <optional>
```

- `postgres_parser.hpp` 与 `nodes/*`：来自 DuckDB/libpg_query，用于解析 PostgreSQL 语法。
- `b_plus_tree.h`, `table.h`, `catalog.h`, `page_manager.h`：存储层。
- `log_manager.h`, `lock_manager.h`, `mvcc.h`：事务、锁和 MVCC。
- `query_utils.h`：WHERE/JOIN 条件求值等辅助函数。
- `<optional>`：用来表示当前会话是否在一个事务中，以及事务快照是否存在。

---

## 2. 函数声明：`ExecuteQuery`

```cpp
bool ExecuteQuery(const std::string& query,
                  duckdb::PostgresParser& parser,
                  toydb::Catalog& catalog,
                  toydb::PageManager& page_manager,
                  toydb::LogManager& log_manager,
                  toydb::LockManager& lock_manager,
                  toydb::MVCCManager& mvcc_manager,
                  std::optional<int>& session_tx_id,
                  std::optional<toydb::TxId>& session_snapshot);
```

作用：  
**执行一条完整的 SQL 语句**。返回值含义：

- `false`：表示用户发出了退出命令（QUIT/EXIT/.EXIT），上层 REPL 应该结束。
- `true`：其他情况（正常执行 / 错误），继续下一条命令。

参数说明：

- `parser`：全局复用的 DuckDB Postgres 解析器。
- `catalog`：当前数据库的目录（所有表的集合）。
- `page_manager`：页面管理器，用于读写磁盘页。
- `log_manager`：记录事务 BEGIN/COMMIT/ROLLBACK 以及逻辑日志。
- `lock_manager`：用于加表/行锁，实现 2PL。
- `mvcc_manager`：维护提交事务集合和快照，用于判断版本可见性。
- `session_tx_id`：当前会话活跃事务 ID，如果没有事务就是 `nullopt`。
- `session_snapshot`：当前事务开始时的快照（最大已提交 TxId），非事务状态下为空。

---

## 3. `ExecuteQuery` 内部结构

### 3.1 预处理与退出命令

```cpp
std::string trimmed_query = toydb::Trim(query);
if (trimmed_query.empty()) {
    return true; // 空语句直接继续
}

std::string upper_query = trimmed_query;
std::transform(upper_query.begin(), upper_query.end(), upper_query.begin(), ::toupper);
if (upper_query == "QUIT" || upper_query == "EXIT" || upper_query == ".EXIT") {
    return false; // 信号：结束 REPL
}
```

- 利用 `query_utils::Trim` 去掉两端空白。
- 若整行只有 QUIT/EXIT/.EXIT，则直接要求外层退出循环。

### 3.2 解析 SQL

```cpp
parser.Parse(trimmed_query);
if (!parser.success) {
    std::cerr << "Error: " << parser.error_message << std::endl;
    return true;
}

duckdb_libpgquery::PGList* parse_tree = parser.parse_tree;
if (parse_tree == nullptr || parse_tree->head == nullptr) {
    std::cerr << "Error: Empty parse tree." << std::endl;
    return true;
}

duckdb_libpgquery::PGNode* stmt_node = (duckdb_libpgquery::PGNode*)parse_tree->head->data.ptr_value;
if (stmt_node->type != duckdb_libpgquery::T_PGRawStmt) {
    std::cerr << "Error: Invalid statement type." << std::endl;
    return true;
}

duckdb_libpgquery::PGRawStmt* raw_stmt = (duckdb_libpgquery::PGRawStmt*)stmt_node;
```

- 使用 DuckDB 的 `PostgresParser` 解析 SQL。
- 这里只处理 **单条语句** 的情况：取 `parse_tree->head`。
- 要求顶层节点类型是 `PGRawStmt`，否则认为不支持。
- `raw_stmt->stmt` 才是实际语句节点，后续的逻辑都建立在其 `type` 上。

### 3.3 `CREATE TABLE`

```cpp
if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGCreateStmt) {
    auto* create_stmt = (duckdb_libpgquery::PGCreateStmt*)raw_stmt->stmt;
    std::string table_name = create_stmt->relation->relname;
    std::vector<toydb::Column> columns;

    // 遍历列定义列表 tableElts
    for (auto* col_node = create_stmt->tableElts->head;
         col_node != nullptr; col_node = col_node->next) {
        auto* node_data = (duckdb_libpgquery::PGNode*)col_node->data.ptr_value;
        if (node_data->type == duckdb_libpgquery::T_PGColumnDef) {
            auto* col_def = (duckdb_libpgquery::PGColumnDef*)node_data;
            std::string col_name = col_def->colname;
            // 使用 PgValueToString 读取类型名，如 "int4" / "varchar"
            std::string type_name = toydb::PgValueToString(
                (duckdb_libpgquery::PGValue*)col_def->typeName->names->tail->data.ptr_value);

            toydb::ColumnType col_type;
            size_t col_length = 0;

            if (type_name == "int4") {
                col_type = toydb::ColumnType::INT;
            } else if (type_name == "varchar") {
                col_type = toydb::ColumnType::VARCHAR;
                // 解析 varchar(n) 中的 n
                ...
            } else {
                std::cerr << "Unsupported column type: " << type_name << std::endl;
                continue;
            }
            columns.emplace_back(col_name, col_type, col_length);
        }
    }
    auto new_table = std::make_unique<toydb::Table>(table_name, std::move(columns), page_manager);
    catalog.AddTable(std::move(new_table));

    // 立即刷新 catalog 到 CATALOG_PAGE_ID
    toydb::Page* catalog_page = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
    if (!catalog_page) { ... 创建新页并校验 ID ... }
    catalog.Serialize(catalog_page);
    page_manager.FlushPage(toydb::CATALOG_PAGE_ID);

    std::cout << "Table '" << table_name << "' created successfully." << std::endl;
    return true;
}
```

逻辑小结：

- 从 AST 中解析表名、列名及类型。
- 构造 `Table` 实例并注册到 `Catalog`。
- 立即将整个 `Catalog` 序列化写回目录页，确保重启后能恢复元信息。

### 3.4 事务控制：`BEGIN / COMMIT / ROLLBACK`

```cpp
if (raw_stmt->stmt->type == duckdb_libpgquery::T_PGTransactionStmt) {
    auto* tx_stmt = (duckdb_libpgquery::PGTransactionStmt*)raw_stmt->stmt;
    using Kind = duckdb_libpgquery::PGTransactionStmtKind;

    if (tx_stmt->kind == Kind::PG_TRANS_STMT_BEGIN || tx_stmt->kind == Kind::PG_TRANS_STMT_START) {
        if (session_tx_id.has_value()) { ... already active ... }
        else {
            session_tx_id = log_manager.BeginTransaction();
            session_snapshot = mvcc_manager.BeginSnapshot(session_tx_id.value());
        }
    } else if (tx_stmt->kind == Kind::PG_TRANS_STMT_COMMIT) {
        if (!session_tx_id.has_value()) { ... error ... }
        else {
            int tx_id = session_tx_id.value();
            if (log_manager.CommitTransaction(tx_id, page_manager)) {
                lock_manager.ReleaseLocks(tx_id);
                mvcc_manager.OnTransactionCommitted(tx_id);
                session_tx_id.reset();
                session_snapshot.reset();
            }
        }
    } else if (tx_stmt->kind == Kind::PG_TRANS_STMT_ROLLBACK) {
        if (!session_tx_id.has_value()) { ... error ... }
        else {
            int tx_id = session_tx_id.value();
            if (log_manager.RollbackTransaction(tx_id, catalog, page_manager)) {
                lock_manager.ReleaseLocks(tx_id);
                mvcc_manager.OnTransactionRolledBack(tx_id);
                session_tx_id.reset();
                session_snapshot.reset();
            }
        }
    }
    return true;
}
```

要点：

- `BEGIN`：
  - 分配一个新的事务 ID；
  - 记录当下的快照（当前已提交事务的最大 ID）。
- `COMMIT`：
  - 通过 `LogManager` 提交（刷盘 + COMMIT 日志）；
  - 释放所有锁；
  - 告知 `MVCCManager` 事务已提交；
  - 清空会话中的事务上下文与快照。
- `ROLLBACK`：
  - 使用 `LogManager::RollbackTransaction` 按逻辑日志逆向应用 undo；
  - 释放锁，更新 MVCC（回滚并不加入提交集合）。

### 3.5 `INSERT` 执行

以简化后的逻辑描述：

```cpp
if (raw_stmt->stmt->type == T_PGInsertStmt) {
    auto* insert_stmt = (PGInsertStmt*)raw_stmt->stmt;
    std::string table_name = insert_stmt->relation->relname;
    Table* target_table = catalog.GetTable(table_name);

    if (!target_table) { 错误输出; return true; }

    // 当前实现支持 INSERT ... VALUES(...) 形式
    auto* select_stmt = (PGSelectStmt*)insert_stmt->selectStmt;
    auto* values_list = (PGList*)select_stmt->valuesLists->head->data.ptr_value;
    for 每一行:
        构造 new_row_data（字符串组成的向量），第一个字段转为 int 作为主键 key；

        // 需要写锁（如果在事务中）
        int tx_id_for_lock = session_tx_id.value_or(INVALID_TX_ID);
        AcquireRowLock(table_name, key, tx_id_for_lock, WRITE);

        // 在事务中则记录逻辑日志
        if (session_tx_id.has_value())
            log_manager.LogInsert(...);

        // MVCC：构造 VersionedRow
        VersionedRow v;
        v.data = new_row_data;
        v.create_tx = tx_id_for_row;   // 当前事务 ID 或 INVALID_TX_ID
        v.delete_tx = INVALID_TX_ID;
        v.committed = !session_tx_id.has_value();  // 非事务插入立即视为已提交

        // 从 B+ 树中取出原版本链（若存在），追加新版本，删除旧链再插入新链
        std::vector<VersionedRow> version_chain = existing_chain_if_any;
        version_chain.push_back(v);
        target_table->bptree_->Delete(key);
        target_table->bptree_->Insert(key, version_chain);

        // 更新根页 ID，并将 catalog 与所有页刷盘
        ...
}
```

重点：

- B+ 树的 value 是 **版本链**（`vector<VersionedRow>`），INSERT 效果是“在链尾追加一个版本”。
- 在事务内插入的版本 `committed=false`，可见性由 MVCC 与事务 ID 决定。

### 3.6 `SELECT` 执行：JOIN + 单表查询 + 聚合

SELECT 部分是 `ExecuteQuery` 中最长的逻辑，这里按几个层次分解：

1. **解析 FROM 子句，判断是否 JOIN**：
   - 如果 `fromClause` 是 `PGJoinExpr`，则 `is_join = true`，分别解析左、右表名，获取 `Table*` 指针以及 JOIN 的 ON 条件 `join_condition`。
   - 否则视为单表查询。

2. **JOIN 逻辑（当 `is_join && left_table && right_table`）**：
   - 通过 `bptree_->GetAllValues()` 分别取出左右表的所有版本链。
   - 对每个组合 `(left_row, right_row)`：
     - 调用 `FindVisibleVersion` 在各自的版本链中找到对当前事务可见的版本，若任一表在该 key 上不存在可见版本，则跳过。
     - 将两个可见版本的数据拼接形成 `combined_row`。
     - 若 JOIN 有 ON 条件：
       - 用 `EvaluateJoinCondition(join_condition, left_row, right_row, left_table, right_table)` 计算条件，仅当为 true 时加入结果集合。
   - 若存在 WHERE 子句，再用 `EvaluateWhereClause` 在 `combined_row` 上过滤。
   - 最后根据 SELECT 列列表做投影，并打印行。

3. **单表 SELECT（非 JOIN）**：
   - 相比 JOIN，多了一部分对聚合/ORDER BY/GROUP BY 的处理：
     - 在没有聚合时：
       - 从 `GetAllValues()` 取出所有键值（即版本链），结合 `FindVisibleVersion` 和 `EvaluateWhereClause` 做 MVCC + WHERE 过滤；
       - 根据 ORDER BY 对结果集排序；
       - 按 SELECT 列列表做投影。
     - 在有聚合函数（COUNT/SUM/AVG/MAX/MIN）时：
       - 同样先枚举可见行，但不立即输出，而是统计需要的指标；
       - 对 GROUP BY 的场景，按 group key 将行分组，在每个组里做对应聚合。

4. **MVCC 可见性**：
   - 所有基于 B+ 树读取的行，都会尽量通过：
     ```cpp
     const VersionedRow* visible =
         toydb::FindVisibleVersion(version_chain, session_tx_id, session_snapshot, mvcc_manager);
     ```
     先筛掉对当前事务不可见的版本。

### 3.7 `UPDATE` / `DELETE` 执行

两者的结构类似：

- 通过 `bptree_->GetAllValues()` 遍历所有键及其版本链；
- 对每个键：
  - 使用 `FindVisibleVersion` 找出当前事务可见的版本，若不存在则跳过；
  - WHERE 条件不匹配则跳过；
  - 需要修改的行，先申请行级写锁；
  - 在事务上下文中记录日志（`LogUpdate` 或 `LogDelete`）；
  - 对版本链末尾或匹配版本进行“逻辑修改”：
    - UPDATE：将可见版本标记 `delete_tx = tx_id`，然后在链尾追加新版本（`create_tx = tx_id`，`delete_tx = INVALID`）；
    - DELETE：对可见版本的 `delete_tx` 设置为当前事务 ID，表示逻辑删除；
  - 重写整个版本链回 B+ 树，更新根页 ID。

这样，UPDATE/DELETE 都实现为 **追加版本 + 标记删除**，并结合 MVCC 由快照决定可见性。

### 3.8 `DROP TABLE` / `SHOW TABLES`

```cpp
if (raw_stmt->stmt->type == T_PGDropStmt) { ... }
if (raw_stmt->stmt->type == T_PGShowStmt) { ... }
```

- DROP TABLE：从 `Catalog` 删除表，刷新目录页。
- SHOW TABLES：遍历 `Catalog::GetTables()` 打印所有表名。

---

## 4. `main()`：REPL 主循环

```cpp
int main() {
    std::cout << "ToyDB - A Simple Database System" << std::endl;
    std::cout << "Type 'QUIT' or 'EXIT' to exit." << std::endl;

    duckdb::PostgresParser parser;
    toydb::PageManager page_manager("toydb.db");
    toydb::Catalog catalog;
    toydb::LogManager log_manager("toydb.log");
    toydb::LockManager lock_manager;
    toydb::MVCCManager mvcc_manager;

    // 启动时加载 Catalog
    toydb::Page* catalog_page_raw = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
    if (catalog_page_raw != nullptr) {
        catalog.Deserialize(catalog_page_raw, page_manager);
        std::cout << "Database loaded." << std::endl;
    }

    std::string line, query;
    std::optional<int> session_tx_id;
    std::optional<toydb::TxId> session_snapshot;

    while (true) {
        if (std::cin.eof() || !std::cin.good()) break;

        std::cout << "toydb> ";
        std::cout.flush();

        if (!std::getline(std::cin, line)) break;

        query += line;

        // 行级退出（无分号也立即退出）
        std::string trimmed_line = toydb::Trim(line);
        std::string upper_line = trimmed_line;
        std::transform(upper_line.begin(), upper_line.end(), upper_line.begin(), ::toupper);
        if (upper_line == "QUIT" || upper_line == "EXIT" || upper_line == ".EXIT") {
            break;
        }

        // 以 ';' 结束一条完整语句
        if (!line.empty() && line.back() == ';') {
            query = toydb::Trim(query);
            if (!query.empty() && query != ";") {
                if (!ExecuteQuery(query, parser, catalog, page_manager,
                                  log_manager, lock_manager, mvcc_manager,
                                  session_tx_id, session_snapshot)) {
                    break; // ExecuteQuery 返回 false，表示退出
                }
            }
            query.clear();
        } else if (!line.empty()) {
            query += " ";
        }
    }

    // 退出前 flush Catalog 与所有页
    toydb::Page* catalog_write_page_raw = page_manager.FetchPage(toydb::CATALOG_PAGE_ID);
    if (!catalog_write_page_raw) { ... 如果没有则新建目录页 ... }
    catalog.Serialize(catalog_write_page_raw);
    page_manager.FlushPage(toydb::CATALOG_PAGE_ID);
    page_manager.FlushAllPages();

    std::cout << "Goodbye!" << std::endl;
    return 0;
}
```

要点：

- **多行 SQL 支持**：通过累积 `query`，直到遇到分号再调用 `ExecuteQuery`。
- **即时退出**：无论是否带分号，只要用户单独输入 `QUIT/EXIT/.EXIT` 即立刻退出。
- **启动/关闭时的持久化**：
  - 启动：若目录页存在，调用 `Deserialize` 组装 `Catalog`。
  - 退出：无条件将当前 `Catalog` 序列化写回目录页，并刷盘所有页面。

整体上，`main.cpp` 把所有存储、事务、并发控制组件串联在一起，形成一个简单但功能齐全的 SQL 交互式数据库原型。



