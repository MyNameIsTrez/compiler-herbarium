# qbe

A cut-down version of the QBE compiler backend for x86-64: an IL with mutable temporaries, turned into pruned SSA, lowered to the System V ABI as register copies, selected backwards, and given registers by an allocator that works with parallel moves.

## The idea

QBE is a small backend that aims for good code with few passes, each one simple.

- **The IL lets a frontend reassign.** A temporary can be assigned in several blocks. The SSA pass is what makes it single-assignment. A PHI goes into the dominance frontier of the blocks that assign a temporary, but only where the temporary is live on entry (pruned SSA). An assignment whose value never leaves its block just gets a new name. In `loop`, `%last = copy %s` is used only after the loop, so it gets no PHI.
- **The ABI is plain copies.** Parameters are copies from `rdi`, `rsi`, and arguments are copies into them. A call is one instruction with a count, and its result is a copy from `rax`. After this pass, nothing else in the backend knows about the calling convention except the set of registers a call clobbers.
- **Selection goes backwards.** Each block is walked from its end, so a comparison whose only use is the jump of the block is folded into it: `xcmp` and `jlt`.
- **Registers go backwards too.** Blocks are done from the innermost loop outwards, each walked from its end. A use takes a register, a definition frees it, and a hint from a nearby copy pulls a temporary toward the register it is copied to or from. A value live across a call avoids the caller-saved registers, so `n` in `fact` lives in `rbx` across the recursive call and is copied back into `rdi` afterwards.
- **Disagreements become parallel moves.** Where two blocks expect a temporary in different registers, or a PHI needs its argument in another register, the moves go on the edge. If the predecessor has several successors, the moves go in a new block named after the edge, such as `loop_end`.

Every entry is checked against the five programs in `examples/`. The IL after each stage, the dominance frontiers, the register maps at the start and end of every block, the assembly and the bytes are in `dumps/<example>/stages.txt`, and the machine code is in `dumps/<example>/code.bin`.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_il` | Shows the IL as a frontend writes it |
| `stage_ssa` | Dominators and frontiers, pruned PHI insertion, renaming |
| `stage_abi` | Turns parameters, calls and returns into register copies |
| `stage_isel` | Selects instructions backwards and fuses compare and jump |
| `stage_live` | Liveness again, loop depth and use costs |
| `stage_spill` | The hints the spill pass leaves for registers live across a call |
| `stage_rega` | Allocates registers, with hints, parallel moves and edge blocks |
| `stage_emit` | Threads jumps, lays out the frame and writes assembly and bytes |
| `stage_export` | Links the runtime, patches jumps, writes `code.bin` and runs `main` |

## What it leaves out

No parser, no stack slots, no `alias`, `load`, `copy`, `fold`, `gvn` or `gcm`, no address-mode matching, no floats, aggregates or varargs, and no spilling (the program stops if it would need to). Parallel moves must be independent. The real QBE writes assembly text and leaves the rest to an assembler, so the bytes here are this entry's own addition.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to QBE's source, which files were read and which were not, and what was dropped.
