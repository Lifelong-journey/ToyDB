-- 08_concurrent_read_write.sql - 并发读写测试
-- 测试读写并发场景下的数据一致性

CREATE TABLE concurrent_test (id int4, v int4);

-- 初始数据
INSERT INTO concurrent_test VALUES (1, 10);
INSERT INTO concurrent_test VALUES (2, 20);
INSERT INTO concurrent_test VALUES (3, 30);

-- 测试1: 读事务应该看到一致的快照
BEGIN;
SELECT * FROM concurrent_test ORDER BY id;
-- 应该看到: (1, 10), (2, 20), (3, 30)
COMMIT;

-- 模拟写事务更新数据
BEGIN;
UPDATE concurrent_test SET v = 100 WHERE id = 1;
COMMIT;

-- 读事务应该看到更新后的数据
BEGIN;
SELECT * FROM concurrent_test WHERE id = 1;
-- 应该看到: (1, 100)
COMMIT;

-- 测试2: 多个写事务的顺序提交
BEGIN;
UPDATE concurrent_test SET v = 200 WHERE id = 2;
COMMIT;

BEGIN;
UPDATE concurrent_test SET v = 300 WHERE id = 3;
COMMIT;

-- 读事务应该看到所有更新
BEGIN;
SELECT * FROM concurrent_test ORDER BY id;
-- 应该看到: (1, 100), (2, 200), (3, 300)
COMMIT;

-- 测试3: 写事务的修改对自己可见
BEGIN;
UPDATE concurrent_test SET v = 400 WHERE id = 1;
SELECT * FROM concurrent_test WHERE id = 1;
-- 应该看到: (1, 400)
COMMIT;

-- 测试4: 并发插入
BEGIN;
INSERT INTO concurrent_test VALUES (4, 40);
COMMIT;

BEGIN;
INSERT INTO concurrent_test VALUES (5, 50);
COMMIT;

-- 应该能看到所有插入的数据
BEGIN;
SELECT * FROM concurrent_test ORDER BY id;
-- 应该看到所有5条记录
COMMIT;

QUIT;


