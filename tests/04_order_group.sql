-- 04_order_group.sql - ORDER BY 与 GROUP BY 测试

CREATE TABLE sales (id int4, customer varchar(50), amount int4);

INSERT INTO sales VALUES (1, 'Alice', 100);
INSERT INTO sales VALUES (2, 'Bob', 50);
INSERT INTO sales VALUES (3, 'Alice', 150);
INSERT INTO sales VALUES (4, 'Bob', 200);

-- ORDER BY 单列升序与降序
SELECT * FROM sales ORDER BY amount ASC;
SELECT * FROM sales ORDER BY amount DESC;

-- GROUP BY 单列 + 聚合
SELECT COUNT(*), customer FROM sales GROUP BY customer;
SELECT SUM(amount), customer FROM sales GROUP BY customer;

QUIT;
























