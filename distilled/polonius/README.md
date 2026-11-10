# polonius

A cut-down copy of Polonius, the Rust borrow checker written as Datalog: a program analysis whose whole logic is a few rules over tables of facts. It is the only entry that computes with relations and the only one that produces a verdict about a program instead of a program.

## The idea

Most entries here compute with data structures and loops. Polonius splits the work in two.

- **Facts.** The compiler front end only records what it sees, as rows of tables: which point of the control-flow graph follows which (`cfg_edge`), where a borrow creates a loan (`loan_issued_at`), where a variable is used or assigned (`var_used_at`, `var_defined_at`), where a loan stops being tracked because the borrowed variable was assigned (`loan_killed_at`), where an access would break the terms of a loan (`loan_invalidated_at`), and which reference flows into which (`subset_base`). `stage_facts` writes them.
- **Rules.** Everything else is derived, and no rule says how. `stage_liveness` derives which variables are live at a point (used later without an assignment in between), then which origins are live, since an origin is live when a live variable's type mentions it. `stage_loans` derives the subsets between origins, the loans each origin holds (a loan enters at its borrow, flows along subsets and along CFG edges while its origin is live and the loan is not killed), the loans that are live at a point, and the errors: an invalidated loan that is live.
- **An engine runs the rules.** It does not know what a loan is. A rule is a head and a body of atoms, and the engine repeats until nothing new appears. In each round it joins every rule once per body atom, with that atom limited to the rows that are new since the last round, since a rule can only produce a new row from a new row (semi-naive evaluation). The dump lists the rules and the rows each round added.

The result is what the Rust community calls non-lexical lifetimes: a loan lasts as long as some live reference may still hold it, not as long as the scope of the variable. In `loop`, each `&sum` or `&mut i` that the lowering takes is live for one statement, so `i = i + 1` is accepted. The rejected program at the end of every dump copies a reference, uses the original, then assigns the borrowed variable: the loan outlives the original reference through the copy, and `errors` has one row.

Every entry is checked against the five programs in `examples/`. The input facts, the rules, the rows each round and the verdict are in `dumps/<example>/stages.txt`. An accepted program is then run by a small interpreter, which is not part of Polonius, so that the output can be compared with `expected.txt`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: no Rust, no types and no MIR, so the examples are lowered by hand into integers and references, and the facts come from this entry's own reading of the book, not from rustc. One point per statement instead of a start and a middle point, no drops or move analysis, no placeholder subset errors, and none of the optimized variants of the rules.
