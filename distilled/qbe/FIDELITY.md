# Fidelity: qbe

Real design being mimicked: QBE, the compiler backend by Quentin Carbonneaux, for amd64 System V: building SSA from an IL that allows reassignment, the ABI lowering to register copies, instruction selection with compare fusion, and the register allocator with its parallel moves.

Reference material used:

- Source: the QBE mirror at `180d2eb4157b706dc480bce50ff6d03a65ec334b` (a scratch clone of the default branch, whose upstream is c9x.me/git/qbe.git). Files read: `ssa.c`, `live.c`, `rega.c`, `cfg.c` (`fillrpo`, `filldom`, `fillfron`, `loopiter`, `fillloop`, `filldepth`, `simpljmp`), `spill.c` (`fillcost`, `tmpuse`, `sethint`, `dopm`, the main loop and `merge`), `amd64/isel.c`, `amd64/emit.c` (`omap`, `emitf`, `emitins` with the `Omul` and `Osub` special cases, `amd64_sysv_emitfn`, `sysv_framesz`), `amd64/sysv.c` (`selcall`, `selpar`, `selret`, `abi`), `amd64/targ.c`, `amd64/all.h`, the flags in `ops.h`, and `util.c`.
- Not read in depth: `parse.c`, `mem.c`, `alias.c`, `load.c`, `copy.c`, `fold.c`, `gvn.c`, `gcm.c`, `ifopt.c`, `simpl.c`, `abi.c`, `amd64/winabi.c`, the arm64 and rv64 back ends, and the `limit` selection part of `spill.c`. No paper or documentation was read.
- The real tool was not built or run, so there is no dump of it to compare with.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_il` | The IL after parsing | `parse.c` (not read), `fillcfg` in `cfg.c` | A CFG of blocks with ordered instructions, mutable temporaries and a jump per block, in the form `parse.c` leaves them | The parser. The five functions are built by hand. No types beyond word, no data definitions, no aggregates or floats |
| `stage_ssa` | `ssa` | `ssa.c` (`phiins`, `renblk`, `getstk`), `filldom` and `fillfron` in `cfg.c`, `filllive` in `live.c` | Dominators, dominance frontiers, PHIs only where the temporary is live on entry (pruned SSA), a definition that does not leave its block renamed locally, and renaming with a per-temporary stack of names | `promote` (stack slots to temporaries) is not there. QBE pops a stack lazily when a name's block does not dominate the use, and this entry does the same in `getstk`. No frontier de-duplication, and the RPO has no loop-depth tie-break |
| `stage_abi` | `abi` | `selpar`, `selcall`, `selret` and `abi` in `amd64/sysv.c` | Parameters become copies from the argument registers, arguments become copies into them, a call is `call` with a count word, a result is always a copy from `rax` after the call (also a call with no use of the result), and `ret x` becomes `rax = copy x` plus a return that records one live register | Only word-sized integer arguments in the first four registers. No stack arguments, no aggregates, no varargs, no `%al` count, no stack slots for parameters |
| `stage_isel` | `isel` | `amd64/isel.c` | A backward walk over each block, a comparison fused into the jump that is its only use (`xcmp` then a conditional jump), and `copy $sym` becoming an address instruction | Address-mode matching, dead-definition removal in `sel`, the operand swap of a comparison and the non-fused compare paths. A jump on a compare that is not adjacent stops the program |
| `stage_live` | `filllive` and `fillloop` | `live.c`, `fillloop`, `filldepth` and `loopiter` in `cfg.c`, `fillcost` in `spill.c` | Live-in and live-out sets by iteration to a fixed point, loop depth per block, and use costs weighted by depth | The sets are 64-bit masks, so a function has at most 60 temporaries (`MAXT`). Costs are printed but nothing consumes them, because there is no spilling |
| `stage_spill` | `spill` | `spill.c` (`sethint`, `tmpuse`, the main loop around calls, `merge`) | What spill leaves for the allocator when nothing is spilled: a temporary live across a call gets the caller-saved registers marked as to be avoided, and the `out` set of a return block names the return register | The whole of spilling (`limit`, choosing victims, reloads and stores, stack slots). `limit` selection was not read. The real pass is one with the spilling it drops, this entry shows only the part whose effect it can show |
| `stage_rega` | `rega` | `rega.c` (`prio1`, `prio2`, `hint`, `carve`, `doblk`, `dopm`, `pmgen`, `move`, edge blocks in the main function) and `util.c` | Blocks ordered by loop depth then id, descending. Hints from copies, from registers and from the other side of a PHI. A backward walk where a use takes a register and a definition frees it, `move` relocating a temporary out of a register that something else needs, runs of copies from registers treated as one parallel move (`dopm`), and per-edge moves placed in a new block named after the edge when the predecessor has more than one successor | Step 3 of the main function (copies shared by all edges into a block with several predecessors), the hint swap for a hinted temporary, `prio1` argument ordering, the ordering and cycle breaking (swaps) of parallel moves (the moves here must be independent or the program stops), RBP and RSP as global registers (here they are simply not allocatable), floats and the register classes. Registers are tried in id order RAX upwards, skipping those to avoid |
| `stage_emit` | `emitfn` and `simpljmp` | `simpljmp` in `cfg.c`, `amd64_sysv_emitfn`, `sysv_framesz`, `emitins`, `emitf` and `omap` in `amd64/emit.c` | Jump threading through empty blocks with union-find, a fall-through when the next block is the target, a conditional jump swapped if its second target follows, the prologue `push rbp; mov rbp, rsp`, the frame padded for the callee-saved count and the saves of the used `rbx`.. registers, the epilogue, and the two-address rewrite (with the swap of a commutative operand and the three-operand `imul` by a constant) | QBE only prints assembly text and leaves assembling and linking to other programs. This entry also encodes x86-64 bytes by hand (registers below r8, 8-bit immediates only) and prints every label where QBE prints only the needed ones. No leaf-function frame omission, no `.p2align`, no data section. A subtraction of two registers and the negation trick stop the program |
| `stage_export` | The assembler, linker and loader (outside QBE) | Not in the real source | Nothing of QBE. The code of every function, a small runtime (`print_int`, `print_str`, copied from the clang entry) and the string go into one buffer, the `rel32` fields are patched, and `main` is run from an `mmap`ed page | All of it. This is this entry's own stand-in for `as`, `ld` and a C runtime |

## Deliberate deviations

- The IL is built by hand by one builder per example, not parsed, because the repo leaves the parser out. The builders write what a frontend would: `arithmetic` assigns `%a` three times in one block, `loop` has mutable `%s` and `%i`, and a copy of `%s` made in the loop block that only the exit block uses, so that the pruning of PHIs is visible.
- `stage_spill` shows a hint step instead of spilling, because with at most a handful of live values no example needs a spill. Dropping it entirely would have lost where the caller-saved set gets excluded.
- The allocatable registers are the ones the encoder can name (below r8), and `rbp` and `rsp` are never allocated.
- Assembly text and bytes both come out of `stage_emit`. The real QBE stops at text.
- A function needs at most 60 temporaries and a few blocks. Self-loops of empty blocks and a conditional jump with equal targets are not handled.
- The real tool was never run, and the review below is a self review.

## Comparison with the real tool

The real tool was not built, so this entry has not been compared with the output of `qbe`. An independent review should write the IL of the five examples, run `qbe -t amd64_sysv` on it, and compare the assembly with the text in `dumps/<example>/stages.txt` under `stage_emit`. The expected agreement is in the shape: the same moves around calls, `ret` through `rax`, the same use of `rbx` for a value live across a call, and a block named `<pred>_<succ>` for an edge that needs moves.

- Command: `qbe -t amd64_sysv < program.ssa` (not run)
- Matches: the stages and their order, the data between them, the register choices that follow from the read code.
- Differs: register numbering and label names are this entry's own, and the instruction bytes do not exist in QBE.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| 2026-11-10 | The agent that wrote the entry (self review, no independent reviewer) | The source files listed above | The generated code was disassembled and checked by hand, and the stage behavior was compared with the read code of `ssa.c`, `rega.c` and `spill.c`. The parts listed as not read were not checked. An independent review is still open |
