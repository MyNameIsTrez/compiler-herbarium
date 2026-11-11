# Fidelity: sea-of-nodes

Real design being mimicked: the compiler of Cliff Click's [Simple](https://github.com/SeaOfNodes/Simple) tutorial, as it is at chapter 21 and later, where it has a machine back end for x86-64.

Reference material used:

- Source: [SeaOfNodes/Simple](https://github.com/SeaOfNodes/Simple) at commit `394f0aba21eb7a73a00014b3ffcf2ad21eeb8e21`, directory `chapter21/src/main/java/com/seaofnodes/simple/`.
- Read in depth: `codegen/CodeGen.java` (the phases), `Parser.java` (loops, branches, returns, calls), `codegen/GlobalCodeMotion.java`, `codegen/RegAlloc.java`, `codegen/BuildLRG.java` and `codegen/IFG.java`.
- Skimmed only: `codegen/InstSelect.java` (it hands each node to the CPU class's `instSelect`) and the head of `codegen/ListScheduler.java`.
- Not read: the x86-64 node classes, `Encoding.java`, `Coalesce.java`, `LinkMem.java`, `ElfFile.java`, `TypeCheck.java`, `IterPeeps.java` and the loop tree code. Their stages here are modeled on the names and order of the phases in `CodeGen.java`, not on their source.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_parse` | Parse | `Parser.java`, `node/*` | One graph for control and data. The Fun, Parm, Loop, Region, Phi, If, CProj, Call, CallEnd, Result, Return and Stop nodes, with control as input 0. A Loop gets a Phi for every variable up front and fills in the back edge at the end of the loop. A function with several returns gets one Return over a Region and a Phi. Constants hang off the function's entry node and are folded and shared as the nodes are made | The tokenizer and the parser: the examples are hardcoded as the calls the parser would make. No types, memory, structs, arrays or `Start`/`Stop` projections. The graph is not built through a scope object |
| `stage_iter` | Iter | `codegen/CodeGen.java` (`iter`), `IterPeeps.java` | A Region with a single live input is replaced by that input and its Phis by the matching value. A projection of an If with a constant test is replaced by the control the If followed. Unreachable nodes are dropped | A worklist of peepholes. Here a loop repeats two rewrites until nothing changes. Only dead control is cleaned up in this stage, since the data peepholes already ran while the nodes were made. Loop Phis are never collapsed |
| `stage_loop_tree` | LoopTree | `codegen/CodeGen.java` (`loopTree`) | Loop depth per block, and the dominator tree with its depth, which a Region takes from the lowest common dominator of its inputs | The real loop tree (nested loops, loop heads, exits) was not read. The loop depth here is the number of natural loops that contain the block |
| `stage_inst_select` | InstSelect | `codegen/InstSelect.java`, the CPU class (`x86_64_v2`) | Each ideal node is replaced by a machine node, in place: constants to `movi`, strings to a RIP-relative `lea`, Add and Sub with a constant to `addi` and `subi`, Mul to `imul`, a compare and branch to `cmpi` and `jl`, calls to `call` | Instruction selection by the CPU class's patterns. Here it is a `switch`, and it only knows the shapes the examples contain: a Sub always subtracts a constant, a comparison is always against a constant. No addressing modes, no flags node, no multiple results |
| `stage_schedule` | Schedule (`GCM`) | `codegen/GlobalCodeMotion.java` | Control nodes, Phis, Parms and Results are pinned, the rest floats and ends up in the block of its uses. A use by a Phi counts in the matching predecessor. `breakUpGlobalConstants`: a constant with several uses gets a copy per use | Scheduling early and the walk up from the late block that picks the shallowest loop depth. No example has a value that could move out of a loop, and an entry may not contain code the examples do not run. The lowest common dominator of several uses: after the copies, every floating node has one use. The compare stays in the block of its branch. There are no memory anti-dependences |
| `stage_local_sched` | LocalSched | `codegen/ListScheduler.java` | Order inside a block: Phis first, then Parm and Result, then the floating nodes with their inputs first, the compare, and last the branch, call or return | The list scheduler with its ready counts and priorities was only skimmed. This one is a depth-first walk of the inputs |
| `stage_reg_alloc` | RegAlloc | `codegen/RegAlloc.java`, `BuildLRG.java`, `IFG.java` | Rounds of: build the live ranges (LRGs), build the interference graph, color. LRGs join at Phis and two-address instructions and carry a register mask. A def into one register takes that register from the LRGs live there instead of adding edges. A call takes the caller-save registers from the LRGs live across it. Failure is repaired with copies: before a two-address instruction whose input stays live, and, for an LRG with no register left, after each def into one register and before each use that needs one | Coalescing (`Coalesce.java`), splitting around loops, the other split heuristics, a stack for spilling, floating-point registers and optimistic coloring. A graph that the simple removal cannot color stops the program. Only a two-address instruction can interfere with itself; a Phi that does would loop. The calling convention is one argument in `rdi`, the result in `rax`, `rbx` callee-save and every other register caller-save. The redundant moves that coalescing would remove stay (a `mov` between equal registers is not emitted) |
| `stage_encode` | Encoding | `codegen/Encoding.java` | Machine nodes become bytes, with `rel32` fields for calls, jumps and string addresses patched when all offsets are known. `rbx` is pushed and popped by a function that uses it | Not read. No short jumps, no size-based relaxation of `RIPRelSize`, no ELF. `print_int` and `print_str` are bytes of this entry, where Simple has native functions |
| `stage_export` | Export | `codegen/CodeGen.java` (`exportELF`), `LinkMem.java` | The code is linked and then run | The code goes into an anonymous `mmap`, is made executable with `mprotect` and is called. No ELF file is written. `code.bin` holds the raw bytes |

## Deliberate deviations

- No `Start` projections, no memory edge, no types. The examples have only integers and one string, so the graph has no node for memory or for a type lattice, which are a large part of Simple.
- The examples are not parsed. A function per example builds the graph by calling the same operations the parser calls (`define`, `get`, `set`, `if_begin`, `while_begin`, `call` and so on).
- Global value numbering is a linear table scan. Simple uses a hash table of nodes by structure.
- Only dead control flow is cleaned up after parsing, in `stage_iter`. Region and Phi collapse is done there instead of in the node classes' `idealize` methods.
- A function's only call convention is `rdi` for the argument and `rax` for the result. Simple models arguments and results with machine projections, a return address and callee-save copies in the graph (`CalleeSaveNode`). Here `rbx` is saved by pushing it.
- Constants are `movi`/`lea` nodes even where an immediate form would be shorter, unless the pattern above applies.
- Calls are not inlined.
- A block that ends in a call continues in the CallEnd block, as in Simple, but the call itself is not split into projections.
- The coloring picks the lowest register of the mask. Simple has a preference order.

## Comparison with the real tool

Simple has no command-line driver for these examples. Its tests parse programs in its own language. The graphs for the same programs were reasoned out by hand from the parser, not produced by Simple, so this entry has not been compared with Simple's output.

- Matches: not checked against output.
- Differs: see the deviations.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/sea-of-nodes/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
