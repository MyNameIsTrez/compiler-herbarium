# Instructions for agents

This repo holds heavily cut-down compilers and interpreters. Read `README.md` first, since it defines the layout, the commands and the "done" criteria. This file only adds the rules that are easy to miss.

## Constraints

- Every entry is a single, self-contained `main.c` in C17, for Linux only. Do not add shared C files or headers. Duplicate small helpers instead.
- `gcc distilled/<compiler>/main.c` and `gcc original/<compiler>/main.c` must keep working for every entry.
- Every entry must pass every example in its own `examples/` directory and follow the command-line contract in the README (`./a.out <example> [code.bin]`, exit status 2 for a missing or unknown example). Each example has an `expected.txt`.
- Never edit an example's `expected.txt` by hand. Regenerate it with the commands in the entry's "Regenerating expected.txt" section (real Lua for a `program.lua`, `scripts/gen-kernel.sh` for a `goal.txt`, the real `sqlite3` for a `query.sql`), and give such commands for every new kind of example.
- Only include what makes the design interesting. Leave out the tokenizer and parser, and leave out optimizations that all compilers share, such as generic constant folding, dead code elimination and common subexpression elimination, unless the design is built around them. Ask of each piece whether someone designing a new compiler would learn something from it that they would not from any other entry.
- Keep the full pipeline and simplify each stage. Do not drop a stage that defines the real design, and do not replace it with something trivial. A stage that is not distinctive (see the previous rule) can go. Leave out the advanced parts of a stage instead, such as live-range splitting and coalescing in a graph-coloring register allocator, and list them in `FIDELITY.md`.
- Only open-source designs get a `distilled/` entry. Read the real source before claiming that an entry matches it, and do not rely on memory of how the real compiler works.
- Only add an entry for a design that passes "What earns an entry" in `README.md`. A language with trivial semantics, or a front end over an existing back end, does not. Record such a lead as declined in `RESEARCH.md` instead.

## FIDELITY.md

Each `distilled/<compiler>/` entry has a `FIDELITY.md` that maps its stages to the real compiler.

Update `FIDELITY.md` in the same change as `main.c` when the change:

- adds, removes, renames or reorders a stage,
- changes what a stage keeps or drops compared to the real design, or
- adds, removes or changes a deliberate deviation.

Do not update it for edits that leave all of that alone, such as fixing a comment, reformatting, or renaming a local variable.

`scripts/check-fidelity.sh` checks the mechanical part in CI: every `stage_*` function in `main.c` must appear in the mapping table of `FIDELITY.md`, and the other way around. It cannot tell whether a deviation entry is still accurate, so that is your judgment call.

## Before committing a change to an entry

1. Run `scripts/check-entry.sh <entry-dir>`. If the intermediate form legitimately changed, run it with `--update` and commit the regenerated `dumps/`.
2. Run `scripts/coverage-entry.sh <entry-dir>`.
3. For a distilled entry, run `scripts/check-fidelity.sh <entry-dir>`.
4. Run `scripts/check-cloc.sh --update` and commit the README if the Lines of C column changed.
5. Run `scripts/check-sorted.sh` after adding or moving a table row.
6. When an entry is added, or a lead is promoted to planned or dropped, update `design-space.svg` and run `scripts/check-map.sh`. Planned distilled entries are dashed dots, finished distilled ones are filled dots, and `original/` entries are amber diamonds. Cross out a cell only after adding its reason to the "Cells of the design space map that will stay empty" table in `RESEARCH.md`.

## RESEARCH.md

`RESEARCH.md` lists leads in two tables: leads that do not have an entry yet, and leads that were considered and declined. When you add an entry that comes from a lead, delete that lead's row, since the entry's `FIDELITY.md` records what was read. Mark a lead as read only if you actually read it, not if you only listed it.

Keep the rows of every table in `README.md` and `RESEARCH.md` sorted alphabetically by their first column (the "Examples" table is exempt, since it lists examples in order of difficulty). `scripts/check-sorted.sh` checks this in CI.

## Coverage

Line and branch coverage must stay at 100%. Prefer deleting code that is never reached over excluding it. Use a `GCOVR_EXCL_*` comment only for code that can never run, such as the failure path of a syscall, and say why on the same line.

## Dumps

The dumps in each entry's `dumps/<example>/` directories are committed and must be deterministic: no pointers, timestamps or other run-dependent values.
