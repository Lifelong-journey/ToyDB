-- 07_mvcc_versions.sql - MVCC 版本链测试
-- 测试多版本并发控制：版本链保留历史版本

CREATE TABLE mvcc_test (id int4, v int4);

-- 初始插入
INSERT INTO mvcc_test VALUES (1, 10);

-- 测试1: UPDATE 创建新版本，保留历史版本
BEGIN;
UPDATE mvcc_test SET v = 20 WHERE id = 1;
-- 当前事务应该看到更新后的值
SELECT * FROM mvcc_test WHERE id = 1;
COMMIT;

-- 新事务应该看到最新版本
BEGIN;
SELECT * FROM mvcc_test WHERE id = 1;
-- 应该看到: (1, 20)
COMMIT;

-- 测试2: 多次 UPDATE 创建多个版本
BEGIN;
UPDATE mvcc_test SET v = 30 WHERE id = 1;
UPDATE mvcc_test SET v = 40 WHERE id = 1;
SELECT * FROM mvcc_test WHERE id = 1;
-- 应该看到: (1, 40)
COMMIT;

-- 测试3: DELETE 标记删除，保留历史版本
BEGIN;
DELETE FROM mvcc_test WHERE id = 1;
-- 当前事务应该看不到被删除的数据
SELECT * FROM mvcc_test WHERE id = 1;
COMMIT;

-- 新事务应该看不到被删除的数据
BEGIN;
SELECT * FROM mvcc_test WHERE id = 1;
-- 应该返回 0 rows
COMMIT;

-- 测试4: 回滚 UPDATE，应该恢复到之前的版本
INSERT INTO mvcc_test VALUES (2, 100);
BEGIN;
UPDATE mvcc_test SET v = 200 WHERE id = 2;
SELECT * FROM mvcc_test WHERE id = 2;
-- 应该看到: (2, 200)
ROLLBACK;

-- 回滚后，应该恢复到之前的值
BEGIN;
SELECT * FROM mvcc_test WHERE id = 2;
-- 应该看到: (2, 100)
COMMIT;

-- 测试5: 回滚 DELETE，应该恢复数据
BEGIN;
DELETE FROM mvcc_test WHERE id = 2;
SELECT * FROM mvcc_test WHERE id = 2;
-- 应该返回 0 rows
ROLLBACK;

-- 回滚后，应该能看到数据
BEGIN;
SELECT * FROM mvcc_test WHERE id = 2;
-- 应该看到: (2, 100)
COMMIT;

QUIT;


