# Fidelity: wizard

Real design being mimicked: the side table and in-place interpreter of the Wizard WebAssembly engine, as described in "A Fast In-Place Interpreter for WebAssembly" (OOPSLA 2022).

Reference material used:

- Source: [titzer/wizard-engine](https://github.com/titzer/wizard-engine) at `030f1c23b9146806c4f430b907529e0bdcec4a07` (default branch, shallow sparse clone of `src/engine`). Files read: `src/engine/Sidetable.v3` in full; in `src/engine/CodeValidator.v3` the handling of the control opcodes (block, loop, if, else, end, br, br_if, br_table, return), the helpers for the control stack and the value stack, and the `SidetableBuilder` class; in `src/engine/v3/V3Interpreter.v3` `pushFrame`, `doCallFunction`, `doReturn`, the control opcodes of `execOp`, and `doBranch`, `doGoto`, `doSwitch` and `doCtlXfer`.
- Paper: the abstract, the introduction and the side table section, read earlier and not re-read for this entry.
- Not read: the rest of `CodeValidator.v3` (the type rules of every other opcode, exceptions, typed continuations, GC types, SIMD), the rest of `V3Interpreter.v3`, the binary decoder, and the `x86-64` and `compiler` directories (the JIT tiers).
- Built and run: nothing. Wizard is written in Virgil and was not built.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_input` | the module as the engine receives it | the binary decoder (not read) | Function bodies as raw bytes, with the locals declaration first and the final `end` last, and a signature for each function | No module, no sections, no imports, exports or memories. The bodies are given as byte arrays, since decoding is not the design |
| `stage_validate` | `CodeValidator` with its `SidetableBuilder` | `CodeValidator.v3`, `Sidetable.v3` | A control stack of open blocks (start pc, opcode, parameters, results, the height of the value stack under the parameters, reachable flag), a typed abstract value stack, a polymorphic stack after `br`, `br_table`, `return` and `unreachable`. One four-int entry per branch: `dpc`, `valcount`, `popcount`, `dstp`. Forward references threaded through the `dstp` field and resolved when the label's position is known (`bind`). A loop's branches are bound when the loop ends, to the loop opcode and the side table position at the loop start. An `if` gets an entry that jumps to its else arm; the `else` opcode gets one that jumps to the end; an `if` without an `else` is treated as having an empty one. `br_table` has a count entry and one entry per label | Only i32 and i64, 25 opcodes, i32 locals only. No exceptions, `try`, typed continuations, GC and reference types, SIMD, memory, globals, tables or `call_indirect`. Error messages are plain strings |
| `stage_run` | the in-place interpreter | `V3Interpreter.v3` | A frame with a pc into the original bytes, a side table pointer and a frame pointer. `block` and `loop` only skip their block type. A taken branch adds `dpc` to the pc and `dstp` to the side table pointer and moves `valcount` values down over `popcount` values. `if` consumes its entry when true and takes it when false. A `br_if` that is not taken steps over its entry. `br_table` indexes the entries by the key. A call pushes a frame whose locals start at the arguments, and `end` of the function body returns | No dispatch table, no tiering and no probes. Three traps (unreachable, divide by zero, call stack exhausted) instead of the full reason list |

## Deliberate deviations

- No decoder: an example gives the function bodies as byte arrays, as they sit in a code section. The validator and the interpreter read those bytes directly, as the real ones do.
- Operand types are only i32 and i64, and locals are only i32. Everything the side table needs (block types with parameters and results, the polymorphic stack) is there, and the other value types would add rules without adding to the design.
- `br_table` layout: a count entry holding the number of labels besides the default, then one entry per label with the default last, indexed by `key + 1` or, for a key that is out of range, by `count + 1`. The real entry layout follows the same shape (a count entry, then the entries, with the pc moved by the same index), but its exact index arithmetic was not compared line by line.
- A branch made in dead code can see a stack shorter than the label's arity, which would give a negative `popcount`. This entry clamps it to zero, since the entry can never run. Whether the real validator does the same was not checked.
- The interpreter always slides the kept values with one `memmove`. The real one copies values only when `popcount` is above zero and otherwise just lowers the stack pointer.
- The call stack is limited to 16 frames and the value stack to a fixed array, where the real engine has a configurable depth limit and a growable stack.
- LEB128 integers are limited to nine bytes.
- The dump notation (`entries 0..3`, `dpc`, `dstp`, `keep`, `drop`) is this entry's own.

## Comparison with the real tool

None run: Wizard is not built here. The oracle is V8's WebAssembly implementation through `node`, which checks only the printed results, `trap` and `invalid`, not the side table. The shape of the side table, the order of the entries and the way the interpreter consumes them were compared with `Sidetable.v3`, `CodeValidator.v3` and `V3Interpreter.v3` in the files listed above.

## Regenerating expected.txt

The oracle is a real WebAssembly engine: V8 through `node`. Each `program.js` holds the whole module (the bytes of the function bodies in `main.c`, wrapped in the sections of a module), instantiates it, calls `main`, and prints the results, `trap` when execution throws, or `invalid` when the module does not validate. The bytes in `program.js` and in the `examples` table of `main.c` must agree.

```
for d in distilled/wizard/examples/*/; do node "$d/program.js" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
