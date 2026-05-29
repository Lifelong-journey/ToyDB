-- 03_join.sql - JOIN 与简单 WHERE 组合测试

CREATE TABLE students_j (id int4, name varchar(50), age int4);
CREATE TABLE courses_j (id int4, student_id int4, course_name varchar(50));

INSERT INTO students_j VALUES (1, 'Alice', 20);
INSERT INTO students_j VALUES (2, 'Bob', 21);
INSERT INTO students_j VALUES (3, 'Charlie', 22);

INSERT INTO courses_j VALUES (1, 1, 'Math');
INSERT INTO courses_j VALUES (2, 1, 'Physics');
INSERT INTO courses_j VALUES (3, 2, 'Math');

-- 简单 INNER JOIN
SELECT * FROM students_j JOIN courses_j ON students_j.id = courses_j.student_id;

QUIT;
























