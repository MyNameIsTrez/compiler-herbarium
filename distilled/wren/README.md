# wren

A cut-down copy of the Wren virtual machine: a bytecode interpreter in which `a + b` is a method call, and a class finds a method by indexing an array.

## The idea

Wren's bytecode has no add or compare instruction. An operator is a method named after it, and the VM does the lookup for every call.

- **One symbol table for method names.** A name includes its arity, so `call(_)` and `call(_,_)` are different symbols. The compiler interns each name in a single global table, and the symbol is the index. `a + b` compiles to `CALL_1` with the symbol of `+(_)`.
- **A method array per class, indexed by symbol.** `Num` binds `+(_)`, `-(_)`, `*(_)` and `<(_)` to primitives. A symbol the class does not bind is a hole. The lookup is `class->methods[symbol]`: no hash, no inline cache. The cost is arrays with holes, which `stage_core` prints.
- **Static methods live on a metaclass.** `System.print(_)` and `Fn.new(_)` are bound on the metaclass of `System` and `Fn`. The value `System` is a class object, and its own class is that metaclass.
- **Calling a function is a method call.** `fact.call(5)` is `CALL_1` with the symbol `call(_)`, found on `Fn`. That method is not a primitive that runs the function. The loop sees its type, pushes a call frame on the fiber and carries on, so there is no C recursion. `RETURN` pops the frame and puts the result in the first slot of the callee, where the caller expects it.
- **A fiber holds the stacks.** The value stack and the call frames belong to a fiber, and the loop keeps `frame`, `stack_start`, `ip` and `fn` in locals that it stores and reloads when a frame changes.
- **Top-level variables are module variables.** `var fact = ...` is not a local, so the function body reads `fact` with `LOAD_MODULE_VAR` and needs no upvalue.

The entry is checked against the five programs in [`examples/`](examples/). The Wren source of each, the method tables, each function's disassembly and the stack trace are in `dumps/<example>/stages.txt`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: no scanner or parser (a function per example makes the compiler's calls), no classes defined by the program, no fields, no upvalues, no runtime errors, only the root fiber (no `yield` or `transfer`), and `System.print` is a primitive.
