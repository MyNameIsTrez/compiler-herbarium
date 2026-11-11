# Examples of ebpf-verifier

Each directory holds one small program in kernel-like assembly. `r1` points to a buffer (its size is in the example), `r10` points to the end of a 64 byte stack, and a program must write `r0` before `exit`. The programs are written out in the `examples` table of `main.c`; `program.lua` holds the same text.

- `program.lua` is the oracle. It runs the program in real Lua 5.4 on every input it can have (every value of a loaded byte, the two extremes of a wider load) and prints `accept` if no input makes it misbehave, or `reject` if one does. It does not know the verifier's reasons, only whether the program is really safe, so the verifier must say `reject` wherever the oracle does, and the examples are chosen so that it also accepts wherever the oracle does.
- `expected.txt` is the output of `lua5.4 program.lua`. Regenerate it with the command in `FIDELITY.md`, never by hand.

| Example | What it shows |
|---------|---------------|
| `bounded-index` | A loaded byte masked with `& 7` is `0..7`, so it is a safe index into a 16 byte buffer: the tnum bounds the range |
| `compare-guard` | `if r2 > 7` narrows the fall-through to `0..7` and makes the index safe |
| `counter-loop` | A loop that stores through a pointer moved by its counter, bounded by `!= 16`: walked pass by pass (84 instructions) and accepted |
| `dead-branch` | A condition the ranges already decide: only one side is followed |
| `endless-counter` | A loop that always changes state but never ends: rejected at the complexity limit |
| `exit-without-r0` | Rejected: `exit` with `r0` never written |
| `fixed-out-of-bounds` | Rejected: a fixed offset past the end of the buffer |
| `infinite-loop` | Rejected: the same state is reached again on the same path |
| `loop-overrun` | A bounded loop whose last pass reads past the end: rejected on that pass |
| `negative-index` | Rejected: only the upper bound is tested with a signed compare, so the index can still be negative |
| `pointer-compare` | Rejected: a pointer in a conditional jump |
| `pointer-multiply` | Rejected: arithmetic on a pointer other than adding a number |
| `prune-imprecise` | Two paths set `r3` to different constants, but `r3` never feeds a pointer or a branch, so it is not precise and the second path is pruned |
| `prune-precise` | Two paths set `r3` to different constants and `r3` is then added to a pointer: precision marks it, so the second path is not covered and is walked |
| `prune-stack` | A value stored to a stack slot on one path only: the slots take part in the comparison, and the other path is walked to its end |
| `prune-uninit` | One path writes `r3` and the other does not: a saved state with a register set does not cover one with it unset, as the register is read nowhere it is harmless |
| `refine-matrix` | Several jump operators and operand orders, each narrowing a range differently |
| `scalar-deref` | Rejected: a load through a number |
| `scalar-ops` | Add, subtract, multiply, shifts and bit operations on ranges and known bits |
| `scaled-index` | Products of masked values (`r3 = 2 * (r2 & 3)`, and a second chain with `| 1`): the range and the known bits are computed together and stay inside the buffer |
| `signed-guard` | A byte minus 8 is guarded by two signed compares, `s< 0` and `s> 7`, which leave a range that is contiguous in signed order and safe |
| `stack-out-of-range` | Rejected: a stack access outside `fp-64..fp-8` |
| `stack-spill` | A bounded scalar spilled to the stack and filled again keeps its range, and the slot is marked precise when the filled value is used as an offset |
| `stack-uninit-read` | Rejected: a read of a stack slot that was never written |
| `stack-variable-index` | Rejected: a stack access whose offset is not a constant |
| `unbounded-index` | Rejected: an index with no upper bound |
| `variable-out-of-bounds` | Rejected: an index whose maximum is past the end of the buffer |
