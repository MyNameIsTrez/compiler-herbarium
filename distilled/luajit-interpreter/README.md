# luajit-interpreter

A cut-down assembler interpreter, after LuaJIT 2.1, with the hooks that connect it to a trace compiler.

## The idea

LuaJIT's interpreter is not C. It is one block of x86-64 assembly written for a fixed assignment of registers: the bytecode pointer, the frame base, the constants and the dispatch table each live in a register for the whole run. Nothing is saved across handlers.

- **Dispatch at the end of every handler.** There is no central loop. Each handler ends with the same few instructions that load the next word, split out the opcode and A, advance the pc and jump through a table. Every handler has its own indirect jump, which the branch predictor likes.
- **The table is the mode switch.** To record a trace, the VM replaces every entry of the dispatch table with one routine that calls the recorder first. A second, static table keeps the real handlers, and the instruction that is being recorded runs through it.
- **Twin instructions.** `LOOP` counts how often it runs, `ILOOP` is the same without counting, and `JLOOP` jumps into a trace. The recorder patches the bytecode from one to the other. `FUNCF` and `IFUNCF` are the same for calls.
- **Hot counters.** A small table of 16-bit counters, indexed by a hash of the pc. A counter that underflows leaves the interpreter and calls C.
- **Exits.** A trace that fails a guard jumps to a stub that pushes its number and joins a handler. The handler saves the registers, calls C, and the interpreter resumes at the pc that C returns.

This entry generates that interpreter as x86-64 machine code at startup, with a small assembler, and runs the examples on it. The interpreter is the code in `code.bin`: open it with `objdump -D -b binary -mi386:x86-64 -M intel`. For the `loop` example, the hot loop also gets a trace (made from templates, not an IR), so that the dispatch switching, `JLOOP` and the exit handler all run. The hot threshold is 2 here (56 in LuaJIT). In `call`, `fact` gets hot and is blacklisted.

Every entry is checked against the five programs in `examples/`.

## Stages

- `stage_bytecode`: the bytecode of each function, in LuaJIT's instruction format
- `stage_assemble`: generate the interpreter as x86-64 machine code
- `stage_dispatch`: fill the dispatch tables and the hot counters
- `stage_interpret`: run the program on the generated interpreter
- `stage_record`: while recording, the dispatch table sends every instruction to a C hook
- `stage_trace`: when recording reaches the loop again, make the trace and patch `JLOOP` in
- `stage_exit`: a guard failed, and the exit handler saves the registers and calls into C

The last three run inside `stage_interpret`, called from the generated code, so the dump shows them in the order they happen.

## What it leaves out

Floats, tables, strings beyond a constant, metamethods, varargs, the stack growth and error code, the C API, and every optimization of the trace compiler: the trace has no IR, snapshots or register allocation. Function traces are not made.

## Compared with distilled/luajit

The two entries are the two halves of LuaJIT. [luajit](../luajit/) is the trace compiler (recording into IR, the loop optimization, a backwards assembler with register allocation) on top of a bytecode interpreter written as a C `switch`. This entry is the interpreter as it really is, machine code with pinned registers, dispatch tables and hot counters, and only a template for the trace. Read luajit to see how a trace is made, and this one to see how a trace is started, entered and left.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source, which parts of it were read, and what was dropped.
