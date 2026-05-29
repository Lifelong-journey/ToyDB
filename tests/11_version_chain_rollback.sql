-- 11_version_chain_rollback.sql - 版本链回滚测试
-- 测试版本链在回滚场景下的行为

CREATE TABLE version_test (id int4, v int4);

-- 初始数据
INSERT INTO version_test VALUES (1, 10);

-- 测试1: UPDATE 后回滚，应该恢复到原版本
BEGIN;
UPDATE version_test SET v = 20 WHERE id = 1;
SELECT * FROM version_test WHERE id = 1;
-- 应该看到: (1, 20)
ROLLBACK;

-- 回滚后应该恢复到原值
SELECT * FROM version_test WHERE id = 1;
-- 应该看到: (1, 10)

-- 测试2: 多次 UPDATE 后回滚
BEGIN;
UPDATE version_test SET v = 30 WHERE id = 1;
UPDATE version_test SET v = 40 WHERE id = 1;
SELECT * FROM version_test WHERE id = 1;
-- 应该看到: (1, 40)
ROLLBACK;

-- 回滚后应该恢复到原值
SELECT * FROM version_test WHERE id = 1;
-- 应该看到: (1, 10)

-- 测试3: DELETE 后回滚
BEGIN;
DELETE FROM version_test WHERE id = 1;
SELECT * FROM version_test WHERE id = 1;
-- 应该返回 0 rows
ROLLBACK;

-- 回滚后应该能看到数据
SELECT * FROM version_test WHERE id = 1;
-- 应该看到: (1, 10)

-- 测试4: 混合操作后回滚
BEGIN;
UPDATE version_test SET v = 50 WHERE id = 1;
INSERT INTO version_test VALUES (2, 20);
DELETE FROM version_test WHERE id = 1;
SELECT * FROM version_test;
-- 应该只看到: (2, 20)
ROLLBACK;

-- 回滚后应该恢复到初始状态
SELECT * FROM version_test ORDER BY id;
-- 应该只看到: (1, 10)

QUIT;


