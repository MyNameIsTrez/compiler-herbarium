# wizard

A cut-down copy of the interpreter of the Wizard WebAssembly engine, which runs a module in place on its original bytes and finds its branch targets in a side table built by the validator.

## The idea

Most bytecode interpreters rewrite the code into a private form before running it, so that jumps are plain offsets. WebAssembly is built so that this can be avoided: control flow is structured (`block`, `loop`, `if`), and the validator must walk every function anyway to check types.

- **Labels are depths.** `br 1` names the second block outward, not an address. Finding that block's `end` in the bytes would mean scanning forward and counting nested blocks.
- **The validator knows the targets.** It keeps a stack of open blocks and a typed abstract stack of values. At every branch it knows the target block, how many values the branch carries (the label's arity), and how many values lie between them (the stack height now, minus the arity, minus the height at the target's start).
- **One entry per branch.** It records `dpc` (how far the pc moves), `dstp` (how far the side table pointer moves), `valcount` and `popcount`. A target that comes later is not known yet, so the entries that want it are chained through their `dstp` field and filled in when the `end` is reached. A loop's branches go backward and are filled in when the loop ends. An `if` without an `else` is treated as having an empty `else`.
- **The interpreter keeps two pointers.** Every frame has a pc into the original bytes and a pointer into the side table of its function. A taken branch adds the two deltas and slides `valcount` values down over `popcount` values. A branch that is not taken still moves the side table pointer past its entry, because entries are consumed in the order the code reaches them. `br_table` jumps over the entries of the labels it does not choose.

The dumps in `dumps/<example>/stages.txt` show every instruction with the abstract stack after it and the entries it added, then each function's side table, then every branch the interpreter takes with the deltas it applied.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source. The short version: no binary decoder, only i32 and i64 with 25 opcodes, no memory, globals, tables, exceptions, GC types or SIMD, and no JIT tiers. Wizard was not built, and the oracle is V8's WebAssembly engine through `node`.
