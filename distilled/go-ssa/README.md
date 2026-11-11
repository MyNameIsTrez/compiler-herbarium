# go-ssa

A cut-down copy of the register allocator of the Go compiler: one greedy pass over the whole function, eviction by farthest next use, spills placed afterwards in the dominator tree, and moves added on the merge edges.

## The idea

A graph-coloring allocator builds an interference graph first. The Go allocator never does.

- **One long block.** It walks the blocks in order as if they were one basic block. A value is moved into a register just before the instruction that needs it, and when all registers are taken, the value whose next use is farthest away is kicked out.
- **Distance to the next use.** Before the walk, a fixpoint over the blocks finds how far away the next use of each value is at the end of every block. The likely branch counts as nearer, the unlikely one as farther, and a call adds a lot, because everything in a register dies there.
- **Starting a block.** A block starts with the registers at the end of one of its predecessors. A phi takes the register its input had there. The other predecessors will disagree, which is fixed later.
- **Cheap values are not spilled.** A constant is computed again before each use.
- **Spills are placed afterwards.** When a value is kicked out, the walk only records which part of the dominator tree restores it. Later the store goes in the highest block that dominates all restores and still has the value in a register, which is often much later than the definition, in the one branch that needs it.
- **Shuffle.** For every edge into a merge block, a second pass adds copies, loads and recomputations at the end of the predecessor, so that the registers match what the merge block expects. The predecessor has one successor (no critical edges), so this is always possible.

The machine has two registers and a call clobbers both. The dumps in `dumps/<example>/stages.txt` show the walk, where each spill went and the moves on the loop's back edge.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source. The short version: hand-built SSA, an invented machine, and a shuffle that issues its moves in order, without the waiting and cycle breaking of the real one.
