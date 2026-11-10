# puc-lua

A cut-down copy of the reference Lua 5.4 virtual machine: register bytecode and the dispatch loop.

## The idea

Real Lua compiles source into a `Proto` per function, which holds an array of 32-bit instructions, and runs it in `luaV_execute`. This entry keeps the second half. The prototypes for each example are written down by hand from `luac5.4 -l -l`, using the real opcode numbers and bit layout, and a small dispatch loop executes them.

- **Registers, not a stack.** An instruction names its operands as register numbers, which are slots in the window of the stack that belongs to the running function. `ADD 0 0 1` adds registers 0 and 1 into register 0.
- **Instructions that skip instructions.** `ADD` is followed by an `MMBIN` that handles the case where an operand is not a number. If the operands are integers, `ADD` does the work and skips it. A comparison such as `LT` is followed by a `JMP`, which it either skips or takes.
- **Calls share the loop.** A call to a Lua function pushes a frame and keeps going in the same loop, and a return pops it. Results are moved to where the function was.
- **Upvalues.** `fact` calls itself through an upvalue that points at a register of the main chunk. The `k` flag on the main chunk's `RETURN` closes it, which copies the value into the upvalue.

Every entry is checked against the five programs in `examples/`. The stages, the trace of executed instructions and the encoded words are in `dumps/<example>/stages.txt`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: there is no compiler, only the opcodes the examples execute have handlers, and values are integers and strings.
