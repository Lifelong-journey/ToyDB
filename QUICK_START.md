ToyDB 快速开始指南（含测试与演示脚本）

本指南基于当前版本的 ToyDB，涵盖：

- 如何编译和启动数据库
- 如何一键运行所有回归测试（含并发/MVCC 测试）
- 一套可直接复制执行的 SQL 演示脚本（演示完会清理掉创建的表）

---

## 一、编译与启动数据库

### 1. 编译项目

```bash
cd /root/toy_database/build
cmake ..    # 若已配置过可省略
make -j4
```

### 2. 启动交互式数据库

```bash
cd /root/toy_database/build/src
./toy_db
```

提示：

- 命令行提示符为 `toydb>`。
- 输入 `QUIT;` 或 `EXIT;`（分号可省略）即可退出。
  请运行`rm -f /root/toy_database/build/src/toydb.db /root/toy_database/build/src/toydb.log`来避免一些历史错误数据遗留

---

## 二、运行全部回归测试（含并发/MVCC）

在工程根目录（`/root/toy_database`）下执行：

```bash
cd /root/toy_database/tests
bash run_regression_tests.sh
```

该脚本会自动：

- 重新编译 `toy_db`、`concurrency_test`、`concurrency_detailed_test`
- 运行并发相关 C++ 测试程序：

  - `concurrency_test`
  - `concurrency_detailed_test`
- 依次运行 `tests/01_basic.sql` ~ `tests/12_concurrent_transactions.sql`
- 对比实际输出与基线，最后输出：

  - `== All regression tests passed ==`（所有测试通过）

---

## 三、交互式 SQL 演示脚本（可复制整段执行）

下面是一份包含 基础 CRUD / JOIN / 聚合 / 事务 / MVCC 可见性 的演示脚本。
你可以在 `./toy_db` 交互界面中 **逐段复制执行**，也可以写成一个 `.sql` 文件用 `cat script.sql | ./toy_db` 的方式运行。

演示目标：

- 创建并使用三张表：`students`、`courses`、`accounts`
- 展示 SELECT/WHERE/JOIN/聚合/UPDATE/DELETE
- 展示简单的事务与回滚、快照隔离的效果
- 最后 DROP 掉所有演示表，保持数据库干净

> 注意：每条 SQL 需要以分号 `;` 结尾。

### 1. 创建与基础数据准备

```sql
-- 创建学生和课程表
CREATE TABLE students (id int4, name varchar(50), age int4);
CREATE TABLE courses  (id int4, student_id int4, course_name varchar(50));

-- 插入学生数据
INSERT INTO students VALUES (1, 'Alice',   20);
INSERT INTO students VALUES (2, 'Bob',     21);
INSERT INTO students VALUES (3, 'Charlie', 22);
INSERT INTO students VALUES (4, 'David',   23);

-- 插入课程数据
INSERT INTO courses VALUES (1, 1, 'Math');
INSERT INTO courses VALUES (2, 1, 'Physics');
INSERT INTO courses VALUES (3, 2, 'Math');
```

### 2. 基础查询与 WHERE 条件

```sql
-- 查看所有学生
SELECT * FROM students;

-- WHERE 条件查询
SELECT * FROM students WHERE id = 1;
SELECT * FROM students WHERE age > 21;
SELECT * FROM students WHERE id > 1 AND id < 4;
SELECT * FROM students WHERE id = 1 OR id = 3;

-- 指定列查询
SELECT name, age FROM students;
SELECT name FROM students WHERE age >= 21;
```

### 3. JOIN 查询

```sql
-- 学生与课程的 INNER JOIN
SELECT * FROM students JOIN courses ON students.id = courses.student_id;
```

### 4. 聚合与分组

```sql
-- 简单聚合
SELECT COUNT(*) FROM students;
SELECT SUM(age)  FROM students;
SELECT AVG(age)  FROM students;
SELECT MAX(age)  FROM students;
SELECT MIN(age)  FROM students;

-- 带 WHERE 条件的聚合
SELECT COUNT(*) FROM students WHERE age > 20;
SELECT AVG(age)  FROM students WHERE id > 1;
```

### 5. 更新与删除

```sql
-- 更新一行数据
UPDATE students SET name = 'Alice Updated', age = 25 WHERE id = 1;
SELECT * FROM students WHERE id = 1;

-- 删除一行数据
DELETE FROM students WHERE id = 4;
SELECT * FROM students;
```

### 6. 事务与回滚（演示日志+回滚功能）

```sql
-- 创建一个账户表，用于演示事务与回滚
CREATE TABLE accounts (id int4, owner varchar(50), balance int4);

INSERT INTO accounts VALUES (1, 'Alice', 1000);
INSERT INTO accounts VALUES (2, 'Bob',    500);

SELECT * FROM accounts;

-- 开启事务，进行转账操作，但最后回滚
BEGIN;
UPDATE accounts SET balance = balance - 200 WHERE id = 1;
UPDATE accounts SET balance = balance + 200 WHERE id = 2;

-- 在事务内查看（ToyDB 中，同一事务能看到自己的修改）
SELECT * FROM accounts;

-- 回滚事务，撤销转账
ROLLBACK;

-- 再次查询，余额应恢复到初始状态
SELECT * FROM accounts;
```

### 7. 简单快照隔离演示思路（单会话版）

> ToyDB 中快照隔离是面向事务 ID 和版本链实现的，单会话内可以通过顺序操作模拟“先建快照后再提交”的效果。
> 下面是一种简单的顺序演示（严格并发需配合 C++ 并发测试）。

```sql
-- 清理 accounts 表数据，重新开始
DELETE FROM accounts;
INSERT INTO accounts VALUES (1, 'Alice', 1000);

-- 事务 A 开启并读取（建立快照）
BEGIN;
SELECT * FROM accounts WHERE id = 1;
-- 此时事务 A 的 snapshot 记录了当前已提交版本

-- 模拟“另一个事务 B”在 A 之后修改并提交（在同一会话顺序执行）
 -- 先结束 A，避免混淆会话状态
COMMIT;         
 -- 事务 B
BEGIN;          
UPDATE accounts SET balance = 2000 WHERE id = 1;
COMMIT;

-- 再开启一个新事务 C，应该看到最新余额
BEGIN;
SELECT * FROM accounts WHERE id = 1;
COMMIT;
```

### 8. 查看所有表，然后清理演示数据

在演示最后，清理掉我们创建的几张表，使数据库回到“干净状态”：

```sql
--展示数据持久化功能
EXIT
-- 查看当前所有表和数据
SHOW TABLES;
SELECT * FROM accounts;
SELECT * FROM students;
SELECT * FROM courses;

-- 删除演示中创建的表
DROP TABLE courses;
DROP TABLE students;
DROP TABLE accounts;

SHOW TABLES;
```

---

## 四、脚本方式一次性演示

你也可以将上面的 SQL 片段保存为脚本文件并一次性运行，例如：

```bash
cd /root/toy_database

cat > demo.sql << 'EOF'
-- 上面“三、交互式 SQL 演示脚本”中的所有 SQL 内容整体粘贴到这里
EOF

cd build/src
cat ../../demo.sql | ./toy_db
```

推荐先在交互模式下分段运行，熟悉每一步的输出和含义，再使用脚本一次性回放。