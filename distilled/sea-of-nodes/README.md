# sea-of-nodes

A cut-down version of the compiler in Cliff Click's [Simple](https://github.com/SeaOfNodes/Simple) tutorial: one graph for control and data, scheduled late, colored with a graph-coloring register allocator.

## The idea

Most compilers keep a list of instructions inside each basic block. Here the program is one graph, in which every value, every branch and every call is a node, and the edges say what each node reads. There is no block membership while the graph is being optimized.

- **Control is an edge too.** A node that follows control (a call, a branch, a return) has the control node it follows as input 0. A `Region` merges control and a `Phi` merges the values that arrive there. A `Loop` is a `Region` whose back edge is filled in at the end of the loop body.
- **Data nodes float.** An `Add` is not in a block. Folding constants and sharing equal nodes happen as the nodes are made, so the graph is never larger than it needs to be (see `peephole` in `main.c`).
- **Dead control disappears first.** A `Region` with one live input is that input, and a branch on a constant is the side that is taken. `branch` ends up as a straight line with no `If` in it.
- **Instruction selection rewrites nodes in place.** An `Add` with a constant becomes an `addi`, and everything that used it follows along.
- **Global code motion picks blocks.** Only after selection does every floating node get a block, the one nearest its uses, so a value that one path needs is computed on that path.
- **Registers come from graph coloring.** Values are grouped into live ranges (LRGs). A `Phi` and its inputs share one, and so do a two-address instruction and its first input. Each carries the set of registers it may use. Failure is repaired with copies and a new round, which is how `fact` ends up with its argument in `rbx` across the recursive call.

Every entry is checked against the five programs in `examples/`. The graph after each stage, the register allocation rounds and the encoded bytes are in `dumps/<example>/stages.txt`, and the machine code is in `dumps/<example>/code.bin`.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_parse` | Builds the graph |
| `stage_iter` | Collapses dead control flow and drops unreachable nodes |
| `stage_loop_tree` | Orders the blocks and finds dominators and loop depth |
| `stage_inst_select` | Rewrites ideal nodes into x86-64 machine nodes |
| `stage_schedule` | Puts every floating node in a block |
| `stage_local_sched` | Orders the nodes inside each block |
| `stage_reg_alloc` | Builds live ranges and an interference graph, colors it, adds copies when it cannot |
| `stage_encode` | Emits bytes and patches `rel32` fields |
| `stage_export` | Maps the bytes executable and runs `main` |

## Why Turbofan left this design

V8 used Sea of Nodes for Turbofan and then replaced it with a CFG-based IR, Turboshaft. Their post, [Leaving the Sea of Nodes](https://v8.dev/blog/leaving-the-sea-of-nodes), gives the reasons for JavaScript and WebAssembly:

- **Too complex.** Operations with effects (loads, stores, calls) need effect and control chains, and reasoning about the graph gets hard.
- **Too limited.** Most nodes end up on an effect or control chain, so only the pure ones can float.
- **Too slow to compile.** Visiting a graph in a bad order, cache misses and tracking state made compiles slower, and V8 reports that the new IR halves compile time.

This entry has no loads or stores, so it only shows the part of the design that works well: pure arithmetic floating to where it is used. That is also why it is a good place to try a hybrid, where effectful nodes stay in a CFG and pure ones float.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to Simple's source and what was dropped. The short version: no parser, no types or memory, no coalescing or splitting around loops in the register allocator, and loop-invariant hoisting is missing because no example has an invariant.
