-- 12_concurrent_transactions.sql - 并发事务测试
-- 测试多个事务的顺序执行（模拟并发场景）

CREATE TABLE concurrent_tx (id int4, v int4);

-- 事务1: 插入数据
BEGIN;
INSERT INTO concurrent_tx VALUES (1, 10);
COMMIT;

-- 事务2: 读取数据（应该看到事务1的插入）
BEGIN;
SELECT * FROM concurrent_tx WHERE id = 1;
COMMIT;

-- 事务3: 更新数据
BEGIN;
UPDATE concurrent_tx SET v = 20 WHERE id = 1;
COMMIT;

-- 事务4: 读取数据（应该看到事务3的更新）
BEGIN;
SELECT * FROM concurrent_tx WHERE id = 1;
COMMIT;

-- 事务5: 删除数据
BEGIN;
DELETE FROM concurrent_tx WHERE id = 1;
COMMIT;

-- 事务6: 读取数据（应该看不到被删除的数据）
BEGIN;
SELECT * FROM concurrent_tx WHERE id = 1;
COMMIT;

-- 测试快照隔离：事务7在事务8提交前开始，应该看不到事务8的修改
BEGIN;
SELECT COUNT(*) FROM concurrent_tx;
-- 应该看到: 0
COMMIT;

-- 事务8: 插入新数据
BEGIN;
INSERT INTO concurrent_tx VALUES (2, 30);
COMMIT;

-- 事务9: 应该能看到事务8的插入
BEGIN;
SELECT COUNT(*) FROM concurrent_tx;
-- 应该看到: 1
COMMIT;

QUIT;


