# Examples: kernels

The inputs of an entry that searches for code for a function instead of compiling a program. A superoptimizer's input is a function, not a Lua program, so this entry's examples are kernels. `gso` and `stoke` keep identical copies.

Each kernel has two files:

- `goal.txt` is the definition. Line 1 is `inputs 1` or `inputs 2`, the number of 32-bit inputs, named `a` and `b`. Line 2 is one C expression over `uint32_t a` and `uint32_t b`.
- `expected.txt` is the exact stdout an entry must produce. `scripts/gen-kernel.sh` generates it by compiling the expression with gcc, so it does not rest on any entry. Never edit it by hand. `scripts/check-expected.sh` re-runs the generator in CI.

The test vectors are every combination of `0`, `1`, `2`, `0x7fffffff`, `0x80000000` and `0xffffffff` (the first input varies fastest), then 16 pseudo-random vectors from `state = state * 1664525 + 1013904223`, starting from `state = 1`, using the new state as the value. For two inputs a random vector takes `a` first and then `b`. An entry builds these itself, and prints one line per vector, with the output of the code it found:

```
in0=00000000 in1=00000005 -> 00000000
```

The code that an entry finds is not part of `expected.txt`, because shortest sequences are not unique and a random search may stop at a longer one. It is printed on stderr, which is committed as `dumps/<kernel>/stages.txt`. An entry's README says how long a sequence it reaches for each kernel.

| Kernel | Goal | Shortest known on the toy instruction set of `distilled/gso` |
|--------|------|-----------|
| `abs` | absolute value | 3 |
| `eq0` | `a == 0` as 0 or 1 | 2 |
| `ges0` | `a >= 0` as 0 or 1 | 2 |
| `maxu` | unsigned maximum | 4 |
| `minu` | unsigned minimum | 4 |
| `sign` | signum as -1, 0 or 1 | 3 |

A new kernel is a directory with a `goal.txt`. Generate its `expected.txt` with `scripts/gen-kernel.sh <entry>/examples/<name>`, and add it to the other kernel entry too if it should run it.
