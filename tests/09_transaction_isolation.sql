-- 09_transaction_isolation.sql - 事务隔离级别测试
-- 测试快照隔离避免不可重复读和幻读

CREATE TABLE isolation_test (id int4, v int4);

-- 初始数据
INSERT INTO isolation_test VALUES (1, 10);
INSERT INTO isolation_test VALUES (2, 20);

-- 测试1: 避免不可重复读
-- 在快照隔离下，同一事务内的多次查询应该看到相同的结果
BEGIN;
SELECT * FROM isolation_test ORDER BY id;
-- 第一次查询：应该看到 (1, 10), (2, 20)
COMMIT;

-- 模拟另一个事务更新数据
BEGIN;
UPDATE isolation_test SET v = 999 WHERE id = 1;
COMMIT;

-- 新事务应该看到更新
BEGIN;
SELECT * FROM isolation_test WHERE id = 1;
-- 应该看到: (1, 999)
COMMIT;

-- 测试2: 避免幻读
-- 在快照隔离下，事务应该看不到其他事务插入的数据
BEGIN;
SELECT COUNT(*) FROM isolation_test;
-- 应该看到: 2
COMMIT;

-- 模拟另一个事务插入数据
BEGIN;
INSERT INTO isolation_test VALUES (3, 30);
COMMIT;

-- 新事务应该能看到新插入的数据
BEGIN;
SELECT COUNT(*) FROM isolation_test;
-- 应该看到: 3
COMMIT;

-- 测试3: 事务内的修改对自己可见
BEGIN;
INSERT INTO isolation_test VALUES (4, 40);
SELECT COUNT(*) FROM isolation_test;
-- 应该看到: 4（包括自己插入的）
COMMIT;

-- 测试4: 事务回滚后，其他事务看不到未提交的修改
BEGIN;
INSERT INTO isolation_test VALUES (5, 50);
-- 不提交，直接回滚
ROLLBACK;

-- 新事务应该看不到回滚的数据
BEGIN;
SELECT COUNT(*) FROM isolation_test;
-- 应该看到: 4（不包括回滚的）
COMMIT;

QUIT;


