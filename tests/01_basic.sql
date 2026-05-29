-- 01_basic.sql - 基础增删改查与 SHOW TABLES 测试

CREATE TABLE students (id int4, name varchar(50), age int4);
SHOW TABLES;

INSERT INTO students VALUES (1, 'Alice', 20);
INSERT INTO students VALUES (2, 'Bob', 21);
INSERT INTO students VALUES (3, 'Charlie', 22);

SELECT * FROM students;
SELECT * FROM students WHERE id = 2;

UPDATE students SET age = 23 WHERE id = 1;
SELECT * FROM students WHERE id = 1;

DELETE FROM students WHERE id = 3;
SELECT * FROM students WHERE id = 3;

QUIT;
























