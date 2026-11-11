# Fidelity: polonius

Real design being mimicked: Polonius, the next-generation Rust borrow checker, as the `Naive` variant of `polonius-engine` and the book describe it.

Reference material used:

- Source: [rust-lang/polonius](https://github.com/rust-lang/polonius) at `bfd4c945b7713d0e66a3cde80475d9a4494740df` (the commit the abbreviated hash `bfd4c945` names, downloaded file by file with no clone). Files read: `polonius-engine/src/output/naive.rs` in full, `polonius-engine/src/output/liveness.rs` (`compute_live_origins`, the part that holds the rules), `polonius-engine/src/facts.rs` (in an earlier reading), and the book chapters `book/src/rules/relations.md`, `book/src/rules/loans.md` and `book/src/rules/liveness.md` (the last one only says the rules are not yet described).
- Not read: `datafrog` itself (the engine that `naive.rs` calls; only its use in `naive.rs` and `liveness.rs` was read), `polonius-engine/src/output/datafrog_opt.rs`, `location_insensitive.rs`, `initialization.rs`, `output/mod.rs`, the command-line tool in `src/`, the fact-file parser, and everything on the rustc side, `rustc_borrowck` and its code that turns MIR into facts.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | MIR building and the type check that precede the borrow checker | Not in Polonius: rustc builds MIR and relates the types of references | A control-flow graph of statements over locals, references and loans | Everything: the programs are lowered by hand, with a shared borrow for every read of a local and a mutable borrow for every store to an initialized local. See below |
| `stage_facts` | Fact generation, which rustc does and hands to Polonius as `AllFacts` | The relations are in `polonius-engine/src/facts.rs` and `book/src/rules/relations.md`; the generating code is in rustc and was not read | The relations `cfg_edge`, `loan_issued_at`, `loan_killed_at`, `loan_invalidated_at`, `subset_base`, `placeholder`, `var_used_at`, `var_defined_at`, `use_of_var_derefs_origin`, with the meanings the book gives | Start and mid points, paths and move analysis, drops, `known_placeholder_subset`, `universal_region`. How a statement turns into facts is this entry's own. See below |
| `stage_liveness` | Origin liveness | `polonius-engine/src/output/liveness.rs` (`compute_live_origins`) | The rules `var_live_on_entry(V, P) :- var_used_at(V, P)` and `var_live_on_entry(V, P1) :- var_live_on_entry(V, P2), cfg_edge(P1, P2), !var_defined_at(V, P1)`, and `origin_live_on_entry(O, P) :- var_live_on_entry(V, P), use_of_var_derefs_origin(V, O)` | Drop liveness (`var_drop_live_on_entry`, `var_dropped_at`, `drop_of_var_derefs_origin`) and the partly-initialized analysis it needs. A placeholder origin is live at every point, with one more rule here; the book puts that in a disjunct of the loan rules instead |
| `stage_loans` | The loan analysis, `Naive` variant | `polonius-engine/src/output/naive.rs` (`compute`) and `book/src/rules/loans.md` (rules 1 to 8) | The rules `subset` (the base, the closure at a point, the propagation along CFG edges while both origins are live), `origin_contains_loan_on_entry` (the issue, the flow along subsets at a point, the propagation along CFG edges unless the loan is killed or the origin is dead), `loan_live_at` and `errors` | Rule 9 and `subset_errors`, with `known_placeholder_subset` (illegal subset relations between placeholder origins), the removal of subsets of an origin in itself that `naive.rs` does each round, the `LocationInsensitive` and `Opt` variants and the `Hybrid` combination. The engine is a generic rule interpreter, in place of datafrog's typed leapjoins. See below |
| `stage_run` | Not part of Polonius | None | An interpreter for an accepted program | Everything. It exists so that the output can be compared with `expected.txt` |

## Deliberate deviations

- Rust, its types and MIR are left out, and the programs are tiny. The examples are lowered by hand into statements over integer locals and references, so that they contain loans. Reading a local takes a shared borrow and storing to one takes a mutable borrow, then the value moves through the reference. This is not what rustc's MIR looks like for these Lua programs (it would read and store the locals directly). It gives each example loans that start, flow and die, without adding a language.
- One CFG point per statement. Polonius has two for each MIR location, start and mid, and records the facts at one or the other (an issued loan flows from the mid point, for instance). With one point, a loan is contained on entry at the borrow itself and reaches the next statement through the rule that propagates along the edge.
- The statement-to-fact rules are this entry's, written from the book's descriptions of the relations, not from rustc's code. A statement that assigns a local kills its loans (`loan_killed_at`); any access to a local invalidates its loans that conflict (a read conflicts with a mutable loan, a write with every loan), except the loan the same statement issues. These facts are emitted for every loan of the local, wherever the loan is issued, so a loan can be invalidated at a point before it exists. That is harmless, since only a live loan is an error, and the dump shows it, for example `L1 ... invalidated at main:0`. Whether rustc emits the same facts was not checked.
- Loans are on whole locals. There are no paths with fields or derefs, so there are no prefixes of a path and no loans of a place behind a reference.
- No type relates a reference to another, so `subset_base` has one fact for each borrow (the loan's origin flows into the origin of the variable that holds it) and one for each copy of a reference. The closure rule (rule 2) holds only for facts at the same point, and no point has two, so it never derives a row here. The propagation rule (rule 3) derives a row only for the rejected program, where a copy and the original are live together.
- Placeholder origins stand for the region of a reference parameter. Only the liveness rule uses them here. No example has a loan flow into one, and there are no declared bounds between them.
- Drops are left out: integers and references have no destructor, so there is no `var_dropped_at` and no drop liveness.
- The engine rescans whole relations for every join and for every new row (no indexes), and stores rows in fixed arrays of three integers. The semi-naive loop matches datafrog's iteration in structure, but not in its data structures.
- The program that the borrow checker rejects is not one of the examples. It is added to every dump so that the error rule has something to find.
- The verdict is whether `errors` has a row in any function. Polonius reports the rows and leaves the decision to rustc.
- The interpreter keeps locals in one array and a reference is an index into it. There is no machine code, so there is no `code.bin`.

## Comparison with the real tool

None. The Polonius repository has a command-line tool that reads fact files, but it is a Rust workspace and was not built. The rules were compared by reading `naive.rs` and the book.

- Command: none run.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/polonius/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
