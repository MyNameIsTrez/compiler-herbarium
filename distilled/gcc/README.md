# gcc

A cut-down version of GCC: a tree (GENERIC) that is flattened to GIMPLE, split into blocks, put in SSA form, expanded to RTL on pseudo registers, merged by combine, colored by IRA and printed as assembly.

## The idea

GCC has three IRs, and each one is the right form for the work done on it.

- **GENERIC is a tree.** The front end hands over nested expressions with the constants already folded and the `while` loop already rotated.
- **GIMPLE is flat three-address code.** The gimplifier names every intermediate value `_N`, so no statement has more than one operation. A return writes one shared temporary, and lowering turns all returns into a jump to the single real one.
- **The CFG and SSA form hold the middle end.** Blocks come from labels and jumps. PHIs go at the iterated dominance frontier of each assignment, only where the variable is live, and renaming walks the dominator tree.
- **RTL is the machine's view.** Expand puts the names that a PHI joins in one partition and one pseudo register, so that the PHI costs nothing, and writes `set` insns on pseudos. A compare and a conditional jump follow the layout of the next block.
- **Registers come late.** `combine` first adds copies so that no pseudo is tied to a hard register, then merges a copy into its single user. IRA colors the conflict graph. A pseudo that is live across a call conflicts with every caller-saved register, so it ends in `rbx`, and the prologue pushes it.
- **Text out, not bytes.** GCC writes assembly and lets `as` and `ld` finish. This entry's `stage_encode` stands in for them, so that the result can run in memory.

The entry is checked against the five programs in [`examples/`](examples/). Every stage's output is in `dumps/<example>/stages.txt`, and the machine code is in `dumps/<example>/code.bin`.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_ast` | Builds GENERIC in the shape the C front end leaves it |
| `stage_gimplify` | Flattens it into GIMPLE with temporaries, labels and gotos |
| `stage_lower` | Turns every return into a jump to one return |
| `stage_cfg` | Splits it into basic blocks and edges |
| `stage_ssa` | Dominators, frontiers, PHIs and renaming |
| `stage_expand` | Partitions the SSA names and writes RTL |
| `stage_combine` | Adds copies, then merges a copy into its user |
| `stage_ira` | Colors the pseudos with hard registers |
| `stage_frame` | Decides the pushed registers and the stack adjustment |
| `stage_final` | Prints the assembly of each insn |
| `stage_encode` | Encodes the instructions and patches the `rel32` fields |
| `stage_export` | Maps the bytes executable and runs `main` |

## What it leaves out

No front end, and no optimization pass between SSA and expand, so the code has the unoptimized shape (the real compiler folds all of the examples away). No loop optimizer, alias analysis, scheduler, spilling, live range splitting or shrink-wrapping. Only `int` values, and `+`, `*`, and `<` or `<=`. Out of SSA has no copies on edges, because the examples need none.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to GCC's source, which files were read and which were not, and the comparison with the real compiler's dumps.
