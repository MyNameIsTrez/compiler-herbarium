CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INT NOT NULL);
CREATE TABLE orders(id INTEGER PRIMARY KEY, user INT NOT NULL, amount INT NOT NULL);
CREATE INDEX orders_user ON orders(user);
CREATE INDEX users_age ON users(age);
INSERT INTO users VALUES (1,'ann',30),(2,'bob',25),(3,'cy',30);
INSERT INTO orders VALUES (1,1,10),(2,1,20),(3,3,5),(4,2,7);
SELECT amount FROM orders WHERE user = 1 AND amount >= 10 AND amount < 20 AND amount <= 15;
