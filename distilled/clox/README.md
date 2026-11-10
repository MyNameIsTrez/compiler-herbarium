# clox

A cut-down copy of the bytecode interpreter from "Crafting Interpreters": a one-pass compiler that has no syntax tree, and a stack VM with closures.

## The idea

Real clox scans, parses and generates code in one pass. A Pratt parser calls the code generator while it recognizes the source, so there is no AST and no IR between the source and the bytecode. This entry keeps that pass and the VM. Only the scanner is replaced, by the token stream of each example written down by hand.

- **Pratt parsing with code generation inside.** `parse_precedence` looks up a prefix and an infix function for each token in a rules table. `binary` emits its opcode after both operands, so the output is already in stack order.
- **Jumps patched afterwards.** `if` and `while` emit a jump with a placeholder offset, compile the body, and `patch_jump` fills the offset in. A loop emits `OP_LOOP`, which jumps backwards.
- **Locals are stack slots.** The compiler keeps a list of the locals in scope, and a local's index in that list is its slot in the frame. Slot zero holds the function being called.
- **Upvalues.** `fact` calls itself, so its body names a local of the enclosing function. The compiler marks that local as captured, and `OP_CLOSURE` is followed by the slot to capture. At run time the upvalue points at the live stack slot. When the scope ends, `OP_CLOSE_UPVALUE` (or `OP_RETURN`) copies the value into the upvalue and repoints it, so the closure outlives the frame.
- **One loop for all calls.** A call pushes a frame and keeps running in the same loop.

Every entry is checked against the five programs in `examples/`. The Lox tokens, each function's disassembly and the stack trace are in `dumps/<example>/stages.txt`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: no scanner, no globals, strings, classes or garbage collector, calls take one argument, and upvalues only capture a local of the directly enclosing function.
