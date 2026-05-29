-- 10_lock_conflicts.sql - 锁冲突测试
-- 测试不同锁模式之间的冲突和兼容性

CREATE TABLE lock_test (id int4, v int4);

-- 测试1: 写锁互斥
-- 第一个事务获取写锁
BEGIN;
INSERT INTO lock_test VALUES (1, 10);
-- 在提交前，另一个事务的写操作应该被阻塞（在实际并发场景中）
COMMIT;

-- 测试2: 读锁共享
-- 多个读事务应该能够同时读取
BEGIN;
SELECT * FROM lock_test;
COMMIT;

BEGIN;
SELECT * FROM lock_test;
COMMIT;

-- 测试3: 读锁与写锁冲突
-- 读事务
BEGIN;
SELECT * FROM lock_test;
COMMIT;

-- 写事务（应该能够执行，因为读事务已提交）
BEGIN;
UPDATE lock_test SET v = 20 WHERE id = 1;
COMMIT;

-- 验证更新
BEGIN;
SELECT * FROM lock_test WHERE id = 1;
COMMIT;

QUIT;


