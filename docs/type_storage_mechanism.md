# 数据库类型存储机制说明

## 问题

在 ToyDB 中，所有数据（无论是整数还是字符串）最终都以字符串形式存储在 `VersionedRow::data`（`std::vector<std::string>`）中。那么数据库如何知道哪个是整数，哪个是字符串呢？

## 答案：类型信息存储在表结构（Schema）中

### 1. 类型信息的存储位置

类型信息存储在**表的结构定义（Schema）**中，而不是和数据一起存储：

```20:34:src/table.h
enum class ColumnType {
    INT,
    VARCHAR,
    DOUBLE
};

struct Column {
    std::string name;
    ColumnType type;
    size_t length; // For VARCHAR

    Column() : name(""), type(ColumnType::INT), length(0) {} // Default constructor
    Column(std::string name, ColumnType type, size_t length = 0)
        : name(std::move(name)), type(type), length(length) {}
};
```

每个 `Table` 对象都有一个 `columns_` 成员，其中每个 `Column` 对象都包含：
- `name`：列名
- `type`：列类型（`ColumnType::INT` 或 `ColumnType::VARCHAR`）
- `length`：对于 VARCHAR 类型，存储最大长度

### 2. 类型信息的持久化

当创建表时，类型信息会被序列化到磁盘：

```45:46:src/table.cpp
        std::memcpy(data + current_offset, &col.type, sizeof(ColumnType));
        current_offset += sizeof(ColumnType);
```

当数据库启动时，会从磁盘反序列化表结构，恢复类型信息：

```84:85:src/table.cpp
        std::memcpy(&columns_[i].type, data + current_offset, sizeof(ColumnType));
        current_offset += sizeof(ColumnType);
```

### 3. 数据存储方式

实际的行数据以字符串形式存储在 `VersionedRow` 中：

```18:23:src/definitions.h
struct VersionedRow {
    TxId create_tx{INVALID_TX_ID};   // 创建该版本的事务
    TxId delete_tx{INVALID_TX_ID};   // 标记删除该版本的事务（如果未删除则为 INVALID_TX_ID）
    bool committed{false};           // 该版本是否已经提交
    std::vector<std::string> data;   // 实际的行数据（列值）
};
```

例如，对于表：
```sql
CREATE TABLE students (id INT, name VARCHAR(50));
INSERT INTO students VALUES (1, 'Alice');
```

存储的数据是：
- `data[0] = "1"`（字符串形式）
- `data[1] = "Alice"`（字符串形式）

但表结构 `columns_` 中保存了：
- `columns_[0].type = ColumnType::INT`
- `columns_[1].type = ColumnType::VARCHAR`

### 4. 类型信息的使用

当需要进行类型相关的操作时，代码会通过 `Table::GetColumns()` 获取列的类型信息：

#### 示例 1：聚合函数（SUM、AVG、MAX、MIN）

```522:536:src/main.cpp
                std::string result;
                if (aggregate_func == "count") {
                    result = std::to_string(filtered_rows.size());
                } else if (aggregate_func == "sum") {
                    if (col_idx < table_columns.size()) {
                        double sum = 0.0;
                        for (const auto& row : filtered_rows) {
                            if (col_idx < row.size()) {
                                try {
                                    sum += std::stod(row[col_idx]);
                                } catch (...) {}
                            }
                        }
                        result = std::to_string(sum);
                    }
```

在执行 `SUM(age)` 时：
1. 通过 `table_columns[col_idx]` 找到对应的列定义
2. 虽然代码中可以通过 `table_columns[col_idx].type` 检查类型，但当前实现直接尝试将字符串转换为数值
3. 使用 `std::stod()` 将字符串转换为 `double` 进行计算

#### 示例 2：ORDER BY 排序

```693:709:src/main.cpp
                    if (order_col_idx != SIZE_MAX) {
                        order_desc = (sort_by->sortby_dir == duckdb_libpgquery::PG_SORTBY_DESC);
                        std::sort(result_rows.begin(), result_rows.end(),
                                  [order_col_idx, order_desc](const std::vector<std::string>& a,
                                                              const std::vector<std::string>& b) {
                                      std::string av = (order_col_idx < a.size()) ? a[order_col_idx] : "";
                                      std::string bv = (order_col_idx < b.size()) ? b[order_col_idx] : "";
                                      // Try numeric compare first
                                      try {
                                          double ad = std::stod(av);
                                          double bd = std::stod(bv);
                                          return order_desc ? ad > bd : ad < bd;
                                      } catch (...) {
                                          return order_desc ? av > bv : av < bv;
                                      }
                                  });
                    }
```

排序时，代码会：
1. 先尝试将字符串转换为数值进行比较（适用于 INT 类型）
2. 如果转换失败，则按字符串比较（适用于 VARCHAR 类型）

### 5. 当前实现的局限性

**注意**：当前实现存在一个潜在问题：代码**没有严格检查列类型**，而是依赖运行时转换。

例如，在聚合函数中：
- 代码直接使用 `std::stod()` 尝试转换，如果转换失败就忽略（try-catch）
- 理想情况下，应该先检查 `table_columns[col_idx].type == ColumnType::INT`，然后再进行数值转换
- 对于 VARCHAR 类型的列，不应该执行 SUM、AVG 等数值聚合操作

### 6. 改进建议

可以在执行聚合函数前添加类型检查：

```cpp
// 伪代码示例
if (aggregate_func == "sum" || aggregate_func == "avg") {
    if (table_columns[col_idx].type != ColumnType::INT && 
        table_columns[col_idx].type != ColumnType::DOUBLE) {
        std::cerr << "Error: Cannot apply " << aggregate_func 
                  << " to VARCHAR column" << std::endl;
        return true;
    }
}
```

## 总结

1. **类型信息存储在表结构（Schema）中**：每个 `Table` 的 `columns_` 成员保存了每列的类型信息
2. **数据以字符串形式存储**：所有数据都存储在 `VersionedRow::data` 中，以 `std::vector<std::string>` 形式
3. **类型信息用于运行时转换**：当需要进行类型相关操作时，通过 `GetColumns()` 获取类型信息，然后将字符串转换为相应类型
4. **类型信息会被持久化**：表结构（包括类型信息）会被序列化到磁盘，数据库启动时会恢复

这种设计的好处是：
- 简化了存储层：所有数据统一以字符串形式存储
- 类型信息集中管理：在表结构中统一维护
- 灵活性：可以在运行时根据类型信息进行不同的处理

缺点是：
- 需要额外的类型转换开销
- 当前实现缺少严格的类型检查，可能导致运行时错误

