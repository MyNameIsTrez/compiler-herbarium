# sqlite-vdbe

A cut-down copy of the query planner of SQLite and of the virtual machine it targets: the WHERE clause is turned into a nest of loops, each loop is chosen by a cost model in LogEst, and the loops are compiled to register bytecode for a cursor machine.

## The idea

SQL does not say how to find the rows, so the compiler has to decide.

- **One loop per table.** For every table the planner lists the ways to walk it, called WhereLoops: a full scan, a lookup of one rowid, a full scan of an index that already holds every column the query reads (cheaper, because the rows are narrower), an index seek on `key = ?`, and an index range on `key > ?`, `key < ?` or both.
- **Costs are LogEst.** A cost is `10 * log2(n)` as an integer, so a product is a sum and `LogEstAdd` adds two numbers that are given as logarithms. Nothing is measured: a table is assumed to have about a million rows, a key about ten rows, and a range keeps a fraction of them.
- **Not every loop is kept.** A new loop is skipped if an existing one needs no more outer tables, costs no more and returns no more rows, and it replaces the ones it beats in the same way.
- **The path solver.** A loop may need an outer table (`orders.user = users.id` can drive a lookup of `orders` only after a row of `users` is known). One round per table extends the cheapest path of the last round by a loop that has what it needs, so a join of two tables tries both orders and keeps the cheaper.
- **Code generation.** Each level of the plan becomes a seek, a check that ends the loop, the terms the loop could not use, and a `Next`. When an index is used, the reads of its columns and of the rowid are redirected to the index cursor, and a covering index needs no table cursor at all. Rows are fetched from the table lazily, with a `DeferredSeek` that moves the table cursor only when a column of the table is read.
- **The machine.** `stage_run` interprets the bytecode with cursors, registers and a program counter, and prints each row the way `sqlite3` does.

The dumps in `dumps/<example>/stages.txt` show every loop that was offered and whether it was added, skipped or replaced, the best path of each round, the bytecode before and after the index rewrite, and every instruction as it executes.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source. The short version: a fixed schema of two tables with one single-column index each, no parser, sorted arrays instead of b-trees, and no sorting, `IN`, `OR` or statistics. For all 17 examples, every instruction and every plan line equals what `EXPLAIN` and `EXPLAIN QUERY PLAN` of the real `sqlite3` print.
