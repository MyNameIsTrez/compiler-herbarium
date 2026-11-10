# jlox

A cut-down copy of the tree-walking interpreter from "Crafting Interpreters": a syntax tree, a resolver that binds every variable use to a scope distance before the program runs, and a tree walker over a chain of environments.

## The idea

Real jlox runs the syntax tree directly, with no bytecode. This entry keeps the pass that makes that fast enough to be correct: the resolver. Only the scanner and the parser are replaced, by the syntax tree of each example built by hand.

- **A resolver between parser and interpreter.** `stage_resolve` walks the finished tree with a stack of scopes, one per block and one per function. For each use of a variable it counts how many scopes up the declaration is, and stores that number in a side table keyed by the tree node. The tree itself is not changed.
- **Globals are what the resolver did not find.** A name that is in no scope has no entry in the table, and the interpreter looks it up by name in the global environment. Locals never are.
- **Environments are a chain.** A block or a call makes a new environment that links to the one around it. A local is read by going `distance` links up and looking the name up there.
- **Closures keep their environment.** `fun` stores the environment that is current at that moment in the function object. A call makes its environment as a child of that one, not of the caller's, so lexical scope holds.
- **`return` is an exception.** `return` jumps out of however many nested blocks and loops it sits in, back to the call that is running. jlox throws a `Return` object for this, and this entry uses `longjmp`.

Every entry is checked against the five programs in `examples/`. `fact` is a global in `call`, to show an unresolved name, and `branch` and `loop` wrap their locals in a block so that the resolver has scopes to count. The tree, each resolved distance and a trace of every variable access and call are in `dumps/<example>/stages.txt`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: no scanner or parser, no classes, no errors, no `clock`, calls take one argument, and environments are small arrays.
