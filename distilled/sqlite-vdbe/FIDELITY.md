# Fidelity: sqlite-vdbe

Real design being mimicked: the query planner of SQLite (`src/where.c`, `src/wherecode.c`) and the register bytecode that it emits for the VDBE, the virtual machine of SQLite.

Reference material used:

- Source: [sqlite/sqlite](https://github.com/sqlite/sqlite) (the public GitHub mirror) at commit `57520227c95396ce39ba6ecaac8d475b78dd3964` (tag `version-3.54.1`, shallow clone, never built). Files read: `src/where.c` (`whereLoopAddBtree`, `whereLoopAddBtreeIndex`, `whereRangeScanEst`, `whereRangeAdjust`, `whereLoopOutputAdjust`, `whereLoopInsert`, `whereLoopFindLesser`, `whereLoopAdjustCost`, `whereLoopCheaperProperSubset`, `wherePathSolver`, the OpenRead part of `sqlite3WhereBegin`, and `sqlite3WhereEnd` including the pass that turns reads of the table cursor into reads of the index cursor), `src/wherecode.c` (`sqlite3WhereCodeOneLoopStart`), `src/build.c` (`sqlite3DefaultRowEst`, `estimateTableWidth`, `estimateIndexWidth`), `src/util.c` (`sqlite3LogEst`, `sqlite3LogEstAdd`).
- Not read: `src/vdbe.c` (the semantics of the opcodes in `stage_run` come from the `EXPLAIN` output and the documented behavior of the opcodes, not from the source), `src/select.c`, `src/whereexpr.c`, the parser, and everything about `ANALYZE` statistics (`sqlite_stat1`, `sqlite_stat4`), `IN`, `OR`, skip-scan, automatic indexes, `ORDER BY`, `GROUP BY` and virtual tables.
- Built and run: only the installed `sqlite3` shell (3.54.1), as the oracle for the output and for `EXPLAIN`.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_schema` | default statistics of a table and its indexes | `build.c`: `sqlite3DefaultRowEst`, `estimateTableWidth`, `estimateIndexWidth` | 200 (a million rows) per table, 33 (ten rows) per distinct key, the column width guess from the type (text 5, else 1), the row width as a LogEst | `ANALYZE` statistics, unique indexes, indexes on more than one column. The index entries are sorted at startup instead of being a b-tree |
| `stage_query` | parser, name resolution, and the splitting of the WHERE clause into terms | `select.c`, `whereexpr.c` (not read) | The terms of the WHERE clause, each offered to the tables it mentions, and the set of columns of each table that the query reads (`colUsed`) | The parser. The query is given as a table of examples. `AND` only, a term compares a column with a constant or with a column of another table |
| `stage_loops` | `whereLoopAddAll`, `whereLoopAddBtree`, `whereLoopAddBtreeIndex` | `where.c` | One WhereLoop per way to walk one table: full scan of the rowid, `rowid=?`, full scan of a covering index, `key=?`, `key>?`, `key<?`, `key>? AND key<?`. Costs in LogEst (`rRun`, `nOut`) computed by the same formulas, `whereRangeScanEst`, `whereLoopOutputAdjust` (only with more than one table), the dominance test of `whereLoopInsert` and `whereLoopFindLesser` that skips, adds or replaces a loop | See the list of deviations |
| `stage_solve` | `wherePathSolver` | `where.c` | One round per table, extending each kept path by a loop whose prerequisite tables are all outer already, the cost `rRun + nRow` added with `LogEstAdd`, the `rUnsorted - 2` tie-break, the final path chosen by cost | The N best paths per round (`mxChoice`), which only matter with an `ORDER BY` or many tables. One path per set of tables is kept instead |
| `stage_codegen` | `sqlite3WhereBegin`, `sqlite3WhereCodeOneLoopStart`, `sqlite3WhereEnd` | `where.c`, `wherecode.c` | The opening of the cursors in level order (table cursors numbered by position in the FROM clause, index cursors after them), the seek and the end check of each kind of loop (`SeekRowid`, `Rewind`, `SeekGE`, `SeekGT`, `IdxGE`, `IdxGT`), `DeferredSeek`, the terms that the loop does not use coded as `Ne`/`Le`/`Lt`/`Ge`/`Gt` with their constants loaded in the block after `Halt`, the `Next` with its `p3` and `p5` flags, the jump labels (break, continue), the pass that rewrites `Column` and `Rowid` on the table cursor into reads of the index cursor, and the omission of the table cursor for a covering index | Affinity beyond text and integer, collations, `OP_Once`, bloom filters, `LEFT JOIN`, `ORDER BY`, subqueries, register reuse beyond the one temporary register of the residual terms, comments in the listing |
| `stage_run` | the VDBE loop | `vdbe.c` (not read) | A program counter over the instructions, cursors that move on a b-tree (here arrays) with a seek, a step and a rowid, registers, a deferred seek that is resolved only when a column of the table is read | Transactions, the btree layer, the record format, collations, everything not used by these opcodes |

## Deliberate deviations

- No parser: the query is a row of a table of examples, because a tokenizer and parser would teach nothing about the planner.
- No b-tree: tables and indexes are sorted arrays with a position, because the interesting part is how the planner picks and drives a cursor, not how pages are laid out.
- At most two tables: `MAX_FROM` is 2, so the join order is a choice between two orders. The N-best bookkeeping of `wherePathSolver` is gone, since with at most two tables and no `ORDER BY` it cannot change the result.
- One index per table, on one column, with the rowid as its last key. The rowid column is only compared with `=`; the real planner also seeks it by a range.
- `whereLoopAddBtreeIndex` is recursive over index columns and over `IN` lists; here it adds one level at most (an equality, or a lower bound followed by an upper bound).
- `whereLoopAdjustCost` and `whereLoopCheaperProperSubset` are not kept. They only change the estimates when two loops of one table use a subset of each other's terms, which needs an index with more than one column or several indexes on the same column.
- After `whereLoopInsert` replaces a loop, the real code also removes every later loop that the new one beats. With one index per table no example reaches it, so it is not kept.
- `sqlite3LogEst` and `estLog` are only the branches that the examples reach: arguments are above 8, and the row estimate is above 10. `sqlite3LogEstAdd` is complete. The covering index scan is offered without the check that the index is narrower than the table (it is, for both tables), and a range never has to be limited by the floor of 10 rows.
- All `NOT NULL`, so none of the null handling around the start of a range scan (`bSeekPastNull`, `bStopAtNull`) is needed.

## Comparison with the real tool

`EXPLAIN` of the real `sqlite3` shell prints the bytecode, and `EXPLAIN QUERY PLAN` prints the plan.

- Command, for the instructions of one example (the entry's listing is the block after `after the index rewrite:` in `dumps/<name>/stages.txt`):

```
d=distilled/sqlite-vdbe/examples/join
(head -n -1 "$d/query.sql"; echo ".headers off"; echo ".explain off"; echo ".mode list"; echo "EXPLAIN $(tail -1 "$d/query.sql")") | sqlite3 -init /dev/null :memory:
```

  The plan lines are compared the same way, with `EXPLAIN QUERY PLAN`, against the `plan:` lines of `stage_solve`.
- Matches: for all 17 examples, every instruction (address, opcode, p1, p2, p3, p4, p5) and every plan line is identical to the real output. That includes the cursor numbering in joins, the two orders of a join (`SCAN orders` then `SEARCH users USING INTEGER PRIMARY KEY` is chosen over the other order by the same cost comparison), the registers of the constants that are loaded after the program, and the rewrite of `Column` and `Rowid` to `IdxRowid` and the index column. The rows printed on stdout are identical too.
- Differs: the listing has no comment column. The intermediate forms of `stage_schema`, `stage_query`, `stage_loops` and `stage_solve` have no counterpart that SQLite prints without its `.wheretrace` debug build (`SQLITE_DEBUG`), which was not built, so those forms and the LogEst numbers in them were not compared with a real trace.

## Regenerating expected.txt

The oracle is the real `sqlite3` shell (3.54.1 was used). Each `query.sql` holds the schema, the data and one `SELECT`, and `expected.txt` is what the shell prints for it.

```
for d in distilled/sqlite-vdbe/examples/*/; do sqlite3 -init /dev/null :memory: < "$d/query.sql" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
