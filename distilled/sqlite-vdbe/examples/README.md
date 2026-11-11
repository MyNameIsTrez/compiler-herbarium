# Examples of sqlite-vdbe

Each directory holds one query on the same two tables.

- `query.sql` creates the tables and indexes, inserts the rows, and ends with one `SELECT`. It is the input of the real `sqlite3` shell, which produces `expected.txt`. `main.c` does not read it: it has no parser, so the same query is written out in the `examples` table of `main.c` (tables, columns and WHERE terms), and the two must agree.
- `expected.txt` is the output of `sqlite3 :memory: < query.sql`. Regenerate it with the command in `FIDELITY.md`, never by hand.

The schema is the same in every example, in this order, because the order fixes the root page numbers that `OpenRead` shows:

```sql
CREATE TABLE users(id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INT NOT NULL);
CREATE TABLE orders(id INTEGER PRIMARY KEY, user INT NOT NULL, amount INT NOT NULL);
CREATE INDEX orders_user ON orders(user);
CREATE INDEX users_age ON users(age);
```

| Example | What it shows |
|---------|---------------|
| `scan` | No index fits, so a full scan, with the comparison coded as `Ne` and its constant loaded after the program |
| `rowid` | `id = ?` seeks the table by rowid (`SeekRowid`), with no loop |
| `rowid-miss` | The same seek finds nothing and jumps out |
| `index` | `age = ?` on an index, with `DeferredSeek` to the table |
| `index-miss` | The index seek runs past the last entry |
| `cover` | The index holds every column that is read, so the table is never opened and the rowid comes from `IdxRowid` |
| `cover-scan` | No WHERE clause, but a full scan of the narrower index is cheaper than a scan of the table |
| `range` | `age > ?`: `SeekGT` and no end check |
| `below` | `age <= ?`: `Rewind` on the index and an `IdxGT` end check |
| `between` | A lower and an upper bound in one index range (`SeekGE`, `IdxGE`) |
| `between-gt` | The same with `>` and `<=` (`SeekGT`, `IdxGT`) |
| `residual` | The index serves `user = ?`, and `amount > ?` is checked on every row |
| `residual-ops` | Every comparison operator as a residual check, with one temporary register reused |
| `join` | Two tables with no index on the outer one: the planner scans `orders` and looks `users` up by rowid |
| `join-filter` | A term on the outer table is checked before the inner lookup is made |
| `join-index` | Both tables use an index, and the key of the inner seek is read from the outer index with `IdxRowid` |
| `join-rowid` | The same join, where `orders` is read through its covering index |
