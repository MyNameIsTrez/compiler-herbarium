# Fidelity: go-ssa

Real design being mimicked: the register allocator of the Go compiler (`cmd/compile/internal/ssa/regalloc.go`) and the passes around it.

Reference material used:

- Source: [golang/go](https://github.com/golang/go) at tag `go1.24.0`, commit `3901409b5d0fb7c85a3e6730a59943cc93b2835c` (files from `src/cmd/compile/internal/ssa/`). The default branch has stubs for these files, so the tag was used. Files read: `regalloc.go` (the header comment, `allocReg`, `makeSpill`, `allocValToReg`, `init`, `addUse`, `advanceUses`, `dropIfUnused`, `liveAfterCurrentInstruction`, the main `regalloc` loop, `placeSpills`, `shuffle` with `edgeState`, `computeLive`, `desiredState`), `critical.go`, the start of `layout.go`, the `passes` list in `compile.go`, the start of `README.md`.
- Not read: `stackalloc.go`, `schedule.go`, `tighten.go`, `likelyadjust.go`, `flagalloc.go`, the rewrite rules, and the architecture-specific parts of `regalloc.go`.
- Not built or run: the real compiler was not compiled, so its output was not compared with this entry's.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | Parsing | `syntax`, `noder` | The Lua source of the example, shown as text | The parser: the source is not read by anything |
| `stage_build` | SSA construction and the generic passes | `ssagen`, `ssa/compile.go` (`critical`, `layout`, `schedule`) | SSA values in blocks with phis, blocks in the order the allocator walks them, no critical edges | The passes themselves: the SSA of each example is built by hand in its final shape |
| `stage_live` | `computeLive` | `ssa/regalloc.go` | Distance to the next use at the end of every block, found by a fixpoint, smaller on the likely branch and larger on the unlikely one, +100 across a call, phi arguments counted at the edge | Per-architecture hints, `desired` registers |
| `stage_regalloc` | The `regalloc` walk | `ssa/regalloc.go` (`regalloc`, `allocReg`, `allocValToReg`, `advanceUses`, `makeSpill`) | One greedy pass over the blocks; the start state taken from a predecessor; phis taking the register of their input; arguments moved into the registers an instruction needs; farthest-next-use eviction; constants issued again instead of spilled; two-address instructions; calls clobbering all registers; a restore range recorded per spilled value | See below |
| `stage_spills` | `placeSpills` and `stackalloc` | `ssa/regalloc.go` (`placeSpills`), `ssa/stackalloc.go` | The store goes in the highest block of the dominator tree that dominates every restore and has the value in a register, sunk by walking down the dominator tree; one stack slot per spilled value | Loop depth is not compared when sinking; no slot sharing between values that are not live together |
| `stage_shuffle` | `shuffle` and `edgeState` | `ssa/regalloc.go` (`shuffle`, `edgeState.setup`, `process`, `processDest`) | The sources at the end of a predecessor (registers, plus stack slots of spilled values), the destinations the merge block expects, a copy, a load or a recomputation per move, the phi argument renamed to what ends up there | See below |
| `stage_run` | Code generation | `ssa/regalloc.go` output, `ssagen`, the assembler | An interpreter that runs the allocated code on a machine with NREGS registers and poisons the registers a call or a print clobbers, so a missed spill gives a wrong result | The real back end; this is a small interpreter of my own |

## Deliberate deviations

- The machine is invented: NREGS registers (2, so that the examples spill), every call clobbers all of them, and an argument and a result go in R0. The real allocator reads the register set, the clobber masks and the input and output constraints from the architecture description.
- The SSA is built by hand per example, callee first. There is no `critical`, `layout` or `schedule`: the blocks are in the order the allocator walks them, and no edge is critical.
- The examples were chosen to fit the simplified moves of `stage_shuffle`, whose limits are listed below. The entry does not check those limits.
- No memory values, no flag values, no float registers, no `desired` registers (so an argument is not placed where a later instruction wants it), no loop preload, and none of the special cases for the stack pointer, the g register and WebAssembly.
- The distance to the next use is simplified: the loop-depth and likelihood heuristics of `likelyadjust` are replaced by a `likely` flag on a branch block.
- A phi takes the register of its input in the primary predecessor, or a free register. The real allocator also has phis that live on the stack and a choice among several free registers by desired register. Here a free register is assumed to exist for every phi.
- A block with several predecessors starts from the first predecessor. The real one picks the predecessor with the fewest live spills and records which registers it starts with so the other edges can match them. Here only the phis are moved on a merge edge, and a non-phi value is assumed not to be in a register across a merge.
- When no register is free, the value used farthest away is kicked out. The real allocator first copies it to a free register if one exists and deletes the copy later if nothing uses it. That step, the same treatment of a phi input that other instructions still use, and the copy that keeps the first argument of a two-address instruction alive, are left out. An instruction whose first argument is live afterwards and held in a single register is assumed not to occur.
- The moves of `stage_shuffle` are issued in order. The real `edgeState` makes a move wait while it would overwrite the only copy of a value that is still needed, and breaks a cycle of moves with a temporary register. Here a destination is never the only copy of a value another destination needs. When there are several copies of a value as a source, the first is used instead of one in a register.
- A value is a source on a merge edge if it is live at the end of the predecessor and has a spill; the real code also requires that the spill dominates the edge.
- The distance of a phi argument at an edge is set once; the real code keeps the smaller one when the value is also live there for another reason.
- Size limits are fixed (blocks, values, uses); the real allocator uses slices.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
