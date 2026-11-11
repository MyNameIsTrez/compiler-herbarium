# ghc

A cut-down copy of the back end of the Glasgow Haskell Compiler: STG, the small functional language that comes last before machine code, and the machine that runs it. It is the only entry with lazy evaluation.

## The idea

Haskell does not compute an argument until someone needs it. GHC's answer is to make that explicit in STG, where every argument is an atom, every allocation is a `let` and every evaluation is a `case`. Everything else follows from that.

- **A closure says how it behaves when entered.** A function is `\r`: it can be entered again and again. A thunk is `\u`: it is entered once and then overwritten with its value. A join point is `\j`: it never becomes a heap object, because it is only jumped to. A constructor applied to atoms is not code at all, since it is already a value. `stage_flags` makes these decisions.
- **A closure copies its free variables.** `stage_fvs` annotates each closure with the variables it captures, and leaves out top-level names and join points. A thunk keeps the variables alive that it will need, which is how lazy code leaks space.
- **Only two kinds of frame sit on the stack.** `case` pushes a continuation that remembers its alternatives and environment, then evaluates the scrutinee. Entering a thunk pushes an update frame that remembers the thunk. A function call, a `let` and a jump to a join point push nothing, so a loop written as a join point runs at constant stack depth.
- **A value is returned to the top frame.** An update frame overwrites its thunk with an indirection to the value, so entering the thunk again returns the value at once. A case frame binds the value and picks an alternative.

`call` shows the stack growing with `fact` and the thunk of each `n - 1` being updated. `loop` shows a join point and a chain of thunks that is built lazily: every `sum + i` is a thunk that points at the previous one. Every use of a variable is a new `case`, so the second use of a thunk goes through its indirection. The examples are written that way by hand, not copied from GHC's output.

The entry is checked against the five programs in [`examples/`](examples/). The program after each stage and a trace of the machine are in `dumps/<example>/stages.txt`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: no Haskell, no types and no Core, no optimization at all (so no strictness analysis, no `\s` thunks, no unboxing), no partial applications, no over-saturated calls, no black holes, no garbage collector, and an interpreter in place of the Cmm code generator.
