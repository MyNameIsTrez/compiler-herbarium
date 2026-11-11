# Examples of wizard

Each directory holds one small WebAssembly program, given as the bodies of its functions. `main.c` has no decoder, so the bytes are written out in the `examples` table of `main.c`; `program.js` holds the same bytes inside a whole module, and the two must agree.

- `program.js` is the oracle. It runs the module in V8 through `node` and prints what `main.c` prints: the results of `main` separated by spaces, `trap` if execution traps, or `invalid` if validation rejects the module. It checks results only, not the side table.
- `expected.txt` is the output of `node program.js`. Regenerate it with the command in `FIDELITY.md`, never by hand.

| Example | What it shows |
|---------|---------------|
| `arith` | Straight-line code over the parameters: no control flow, so the side table is empty |
| `block-br` | A forward branch to a block's end that is not taken: the `br_if` entry is skipped |
| `br-drop` | A taken `br_if` keeps one value and drops two below it (valcount 1, popcount 2) |
| `br-table` | A switch: a count entry and one entry per label, indexed by the key |
| `dead-code` | After a `br` the stack is polymorphic, so dead code can pop values that were never pushed |
| `fib` | Recursive calls: every frame has its own pc and side table pointer |
| `i64-wrap` | The validator tracks the type of every value: an i64 is added, then wrapped to an i32 |
| `if-else` | Both arms of an `if` that takes a parameter: false jumps past the `else`, true falls through to it and jumps to the end |
| `if-no-else` | An `if` without an `else`: the validator simulates an empty one, so false jumps to the end |
| `invalid-label` | Rejected by validation: a branch to a label that does not exist |
| `invalid-opcode` | Rejected by validation: an opcode that is not in the instruction set |
| `invalid-type` | Rejected by validation: an i64 where an i32 is expected |
| `loop-params` | A loop with a parameter: a branch to it carries one value |
| `loop-sum` | A loop: `br 0` goes backward to the loop opcode, `br_if 1` forward past the block |
| `multi-result` | A branch out of a block with two results, over one junk value (valcount 2, popcount 1) |
| `return-nested` | `return` from inside two blocks with a value left below the result: it needs no entry |
| `trap-div` | A trap at run time: integer division by zero |
| `trap-stack` | A trap at run time: unbounded recursion exhausts the call stack |
| `trap-unreachable` | A trap at run time: the `unreachable` opcode, which validates because the stack after it is polymorphic |
