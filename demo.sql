-- ToyDB 演示脚本
-- 创建学生表
CREATE TABLE students (id int4, name varchar(50), age int4);

-- 创建课程表
CREATE TABLE courses (id int4, student_id int4, course_name varchar(50));

-- 插入学生数据
INSERT INTO students VALUES (1, 'Alice', 20);
INSERT INTO students VALUES (2, 'Bob', 21);
INSERT INTO students VALUES (3, 'Charlie', 22);
INSERT INTO students VALUES (4, 'David', 23);

-- 插入课程数据
INSERT INTO courses VALUES (1, 1, 'Math');
INSERT INTO courses VALUES (2, 1, 'Physics');
INSERT INTO courses VALUES (3, 2, 'Math');
INSERT INTO courses VALUES (4, 3, 'Chemistry');

-- 查看所有表
SHOW TABLES;

-- 全表扫描
SELECT * FROM students;

-- WHERE条件查询
SELECT * FROM students WHERE id = 1;
SELECT * FROM students WHERE age > 21;
SELECT * FROM students WHERE id > 1 AND id < 4;

-- 范围查询
SELECT name, age FROM students WHERE age >= 21;

-- JOIN查询
SELECT * FROM students JOIN courses ON students.id = courses.student_id;

-- 聚合函数
SELECT COUNT(*) FROM students;
SELECT SUM(age) FROM students;
SELECT AVG(age) FROM students;
SELECT MAX(age) FROM students;
SELECT MIN(age) FROM students;

-- 带WHERE的聚合
SELECT COUNT(*) FROM students WHERE age > 20;

-- 更新数据
UPDATE students SET age = 24 WHERE id = 1;
SELECT * FROM students WHERE id = 1;

-- 退出
QUIT

