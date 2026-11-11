# clang

A cut-down version of the Clang and LLVM pipeline for x86-64: IR with every local in memory, promoted to SSA, selected per block with a DAG, and given registers by the basic allocator.

## The idea

Clang does not try to produce good code in the front end. It emits the dumbest correct IR and leaves the rest to passes, each of which does one job on a form that suits it.

- **Every local lives in memory first.** A variable is an `alloca`, a read is a `load` and a write is a `store`. The front end never has to build SSA or place PHIs.
- **`mem2reg` makes the SSA.** An alloca with one store is replaced by the stored value. Any other alloca gets PHIs where its stores meet, found with the dominance frontier and pruned to the blocks where the value is live. `fact` gets one PHI for its return value, and the loop gets one for each of `sum` and `i`.
- **Selection works on a DAG per block.** Values inside a block are nodes that are shared when equal. Values that cross blocks, and PHI operands, go through virtual registers. Side effects hang on a chain that fixes their order. Each node is then matched to an x86 instruction, and the scheduler walks the chain to write them out.
- **PHIs are lowered late.** Each becomes copies at the end of the predecessors, through a fresh register, so the machine IR leaves SSA only after selection.
- **x86 is two-address.** `A = B + C` is rewritten as `A = B; A += C`, which is why copies appear before every add.
- **Registers come from live intervals.** Each virtual register gets a list of live segments. A coalescer removes the copies whose two sides never overlap. The basic allocator then takes intervals by weight and gives each the first register in the target's order that does not interfere, trying the registers its copies point at first. A call clobbers the caller-saved registers, so a value that lives across the recursive call in `fact` ends up in `rbx`.

The entry is checked against the five programs in [`examples/`](examples/). The IR and machine IR after each stage, the DAG of each block, the intervals, the allocator's decisions and the encoded bytes are in `dumps/<example>/stages.txt`, and the machine code is in `dumps/<example>/code.bin`.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_irgen` | Emits clang-style IR with allocas, loads and stores |
| `stage_mem2reg` | Builds the dominator tree and frontier, places PHIs and renames |
| `stage_isel` | Builds and selects a DAG for each block and writes machine IR |
| `stage_phi_elim` | Replaces PHIs by copies in the predecessors |
| `stage_two_address` | Rewrites `A = op B, C` as `A = B; A = op A, C` |
| `stage_live_intervals` | Numbers instructions and builds the live segments |
| `stage_coalesce` | Joins copies whose intervals do not overlap |
| `stage_regalloc` | Assigns registers by weight, with hints and call clobbers |
| `stage_prologue_epilogue` | Saves and restores the callee-saved registers that were used |
| `stage_encode` | Emits bytes and patches `rel32` fields |
| `stage_export` | Maps the bytes executable and runs `main` |

## What it leaves out

No spilling or live range splitting (the program stops if no register is free), no DAG combines, no other IR passes, no stack frame. The compare in `branch` is of two constants and stays one, because nothing folds it.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to LLVM's source, which files were read and which were not, and what was dropped.
