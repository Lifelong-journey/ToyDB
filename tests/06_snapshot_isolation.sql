-- 06_snapshot_isolation.sql - 快照隔离测试
-- 测试快照隔离：每个事务看到一致的快照视图

CREATE TABLE snapshot_test (id int4, v int4);

-- 初始数据
INSERT INTO snapshot_test VALUES (1, 10);
INSERT INTO snapshot_test VALUES (2, 20);

-- 测试1: 快照隔离 - 事务A在事务B提交前开始，应该看不到B的修改
-- 模拟场景：
-- T1: BEGIN -> SELECT (看到初始数据)
-- T2: BEGIN -> INSERT -> COMMIT
-- T1: SELECT (应该仍然看到初始数据，看不到T2的插入)

BEGIN;
SELECT * FROM snapshot_test ORDER BY id;
-- 此时应该看到: (1, 10), (2, 20)

-- 模拟另一个事务插入数据（在实际并发场景中，这会在另一个线程中发生）
-- 这里我们通过提交当前事务，然后开始新事务来模拟
COMMIT;

BEGIN;
INSERT INTO snapshot_test VALUES (3, 30);
COMMIT;

-- 现在开始新事务，应该能看到所有已提交的数据
BEGIN;
SELECT * FROM snapshot_test ORDER BY id;
-- 应该看到: (1, 10), (2, 20), (3, 30)
COMMIT;

-- 测试2: 可重复读 - 同一事务内多次查询看到相同结果
BEGIN;
SELECT * FROM snapshot_test WHERE id = 1;
-- 在另一个"事务"中更新（模拟）
COMMIT;

BEGIN;
UPDATE snapshot_test SET v = 999 WHERE id = 1;
COMMIT;

-- 新事务应该看到更新后的值
BEGIN;
SELECT * FROM snapshot_test WHERE id = 1;
-- 应该看到: (1, 999)
COMMIT;

-- 测试3: 自己事务的修改对自己可见
BEGIN;
INSERT INTO snapshot_test VALUES (4, 40);
SELECT * FROM snapshot_test WHERE id = 4;
-- 应该能看到自己刚插入的数据
COMMIT;

-- 测试4: 删除操作在快照隔离下的可见性
BEGIN;
SELECT * FROM snapshot_test ORDER BY id;
-- 应该看到所有数据
COMMIT;

BEGIN;
DELETE FROM snapshot_test WHERE id = 2;
COMMIT;

-- 新事务应该看不到被删除的数据
BEGIN;
SELECT * FROM snapshot_test ORDER BY id;
-- 应该看不到 id=2 的数据
COMMIT;

QUIT;


