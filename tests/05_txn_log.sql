-- 05_txn_log.sql - 事务与日志/回滚测试

CREATE TABLE tlog (id int4, v int4);

-- 1. 简单提交事务
BEGIN;
INSERT INTO tlog VALUES (1, 10);
INSERT INTO tlog VALUES (2, 20);
COMMIT;

SELECT * FROM tlog ORDER BY id;

-- 2. 回滚插入
BEGIN;
INSERT INTO tlog VALUES (3, 30);
ROLLBACK;

SELECT * FROM tlog ORDER BY id;

-- 3. 回滚 UPDATE
BEGIN;
UPDATE tlog SET v = 999 WHERE id = 1;
ROLLBACK;

SELECT * FROM tlog WHERE id = 1;

-- 4. 回滚 DELETE
BEGIN;
DELETE FROM tlog WHERE id = 2;
ROLLBACK;

SELECT * FROM tlog ORDER BY id;

QUIT;
























