# pallene

A cut-down version of Pallene, a statically typed sibling of Lua that is compiled ahead of time to C code which runs inside the real Lua runtime. It is the only entry whose subject is how compiled code keeps the garbage collector's roots honest.

## The idea

Lua's collector finds live objects by scanning the Lua stack. A C function that holds a string in a local variable is invisible to it, and the string can be freed under the function's feet. Pallene's compiler solves this at compile time, so that the generated C needs no shadow-stack bookkeeping at run time beyond plain stores.

- **Integers and booleans are C values, strings are collected objects.** `is_gc` splits the types in two. Only variables of a collected type ever need to be visible to the collector.
- **The IR is a flat list of commands in basic blocks.** There is no nesting inside an expression: every intermediate result gets a temporary. `if` and `while` are lowered to `JmpIf` and `Jmp` between blocks, as in a CFG, but there is no SSA and no phi. A `CheckGC` command follows every command that allocates.
- **The collector may run at a `CheckGC` and inside any call.** `stage_gc` runs a backward live-variable analysis over collected variables only and records, at each of these points, which ones are alive.
- **Variables that are alive together get different stack slots.** Slots are handed out greedily, lowest free first, so `line` can reuse the slot of `nl` once `nl` is dead. The largest slot decides the frame size.
- **Every assignment to a variable with a slot also writes the slot.** A variable that dies before the next collection point never has a slot and is never written to the stack. Before a collection point the generated code sets the stack top just above the highest live slot, and the collector scans only below it.
- **A wrong analysis is a crash.** The toy runtime frees every string that the stack below the top does not reach, at every `CheckGC`. A missing mirror write makes a live string disappear and the program faults. Turning the mirror writes off does exactly that on `hello`.

`hello` shows two variables alive at the same time, a slot reused, and a string freed. `call` shows the call as a collection point with nothing to save. The examples are written by hand as typed Pallene programs, so they print through `tostring` and `..` and give the analysis something to find.

Every entry is checked against the five programs in `examples/`. The program after each stage is in `dumps/<example>/stages.txt`.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_source` | Builds the syntax tree of the example |
| `stage_typecheck` | Types every expression and inserts a cast where a value becomes `any` |
| `stage_to_ir` | Lowers to basic blocks of flat commands, with a `CheckGC` after allocations |
| `stage_gc` | Live collected variables at each collection point, then Lua stack slots |
| `stage_coder` | Renders each function as C, with the slot writes and the stack top |
| `stage_run` | Runs the annotated IR on a toy runtime with a collector at every `CheckGC` |

## What it leaves out

No parser, no type errors, no arrays, records, tables, closures, floats or `for` loops, no assignment conversion of captured variables, no uninitialized-variable check and no constant propagation. The C text is printed and not compiled: the same annotated IR is executed by an interpreter in its place.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to Pallene's source, which files were read and which were not, and the list of what was dropped. One finding is worth a look there: in `gc.lua` at the pinned commit every return variable is live throughout its function, whatever its type, which this entry does not copy.
