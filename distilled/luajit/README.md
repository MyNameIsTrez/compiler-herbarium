# luajit

A cut-down tracing JIT, after LuaJIT 2.1.

## The idea

A tracing JIT does not compile functions. It runs a bytecode interpreter, counts how often each loop runs, and compiles only the loops that get hot. It compiles them by recording: the interpreter keeps executing one iteration, and the recorder writes down what it does as a straight line of IR. Where the interpreter made a choice (a branch, the type of a value), the recorded IR holds a *guard* that checks the choice still holds. A trace has no branches, only guards.

- **Snapshots.** A guard that fails must hand control back to the interpreter, which needs its stack as it would have been. Each guard has a *snapshot*: which slots the trace has changed and which IR value holds each one. A slot that still holds what the trace loaded is left out.
- **A loop by copy-substitution.** The recorded iteration is the *pre-roll*. A `LOOP` instruction ends it and a copy follows, with every operand replaced by what it is in the next iteration. A value that changes around the loop gets a `PHI`. There is no separate invariant-code pass: a copy that equals something already in the pre-roll is just not emitted.
- **Assembling backwards.** The assembler walks the IR from the last instruction to the first and writes the code from the end of the buffer. A value gets a register at its last use, which is where the walk first meets it, and gives it back at its definition. The direction of the walk is the liveness analysis.
- **Exits.** A failed guard jumps to a stub that saves the registers and calls a C function, which writes the snapshot's values back into the interpreter's frame and returns the pc to resume at.

The entry is checked against the five programs in [`examples/`](examples/). Only `loop` has a loop that gets hot (the threshold is 3 here, 56 in LuaJIT), so only it makes a trace and a `code.bin`. The others run in the interpreter. The IR, snapshots, registers and bytes of the trace are in `dumps/loop/stages.txt`.

## Stages

- `stage_bytecode`: the bytecode of each function
- `stage_interpret`: run the bytecode, count jumps back, and start a trace when one gets hot
- `stage_record`: record one iteration into IR and snapshots
- `stage_loop`: copy-substitution, with PHIs
- `stage_assemble`: walk the IR backwards, allocate registers and emit x86-64
- `stage_run`: write `code.bin`, make the code executable and run the trace

## What it leaves out

The interpreter is a C `switch`, not the assembler VM of LuaJIT. There are no tables, calls or floats in a trace, no side traces, no trace linking, no spilling, no optimizations except the loop one, and no recovery from a trace that fails to record.

## Compared with distilled/luajit-interpreter

The two entries are the two halves of LuaJIT. This entry is the trace compiler, on top of an interpreter that is a plain C `switch` over 16-byte slots. [luajit-interpreter](../luajit-interpreter/) is the interpreter as it really is: x86-64 machine code with pinned registers, dispatch tables, hot counters and an exit handler, with only a template where the trace compiler would be. Read this one to see how a trace is made, and that one to see how a trace is started, entered and left.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source, which parts of it were read, and what was dropped.
