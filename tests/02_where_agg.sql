-- 02_where_agg.sql - WHERE 组合条件与聚合函数测试

CREATE TABLE numbers (id int4, value int4);

INSERT INTO numbers VALUES (1, 10);
INSERT INTO numbers VALUES (2, 20);
INSERT INTO numbers VALUES (3, 30);
INSERT INTO numbers VALUES (4, 40);

-- 简单 WHERE
SELECT * FROM numbers WHERE value > 15;

-- AND / OR 组合
SELECT * FROM numbers WHERE value > 10 AND value < 40;
SELECT * FROM numbers WHERE value = 10 OR value = 40;

-- 聚合函数
SELECT COUNT(*) FROM numbers;
SELECT SUM(value) FROM numbers;
SELECT AVG(value) FROM numbers;
SELECT MAX(value) FROM numbers;
SELECT MIN(value) FROM numbers;

-- 带 WHERE 的聚合
SELECT COUNT(*) FROM numbers WHERE value >= 20;
SELECT AVG(value) FROM numbers WHERE id > 1;

QUIT;
























