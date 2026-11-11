# ebpf-verifier

A cut-down copy of the verifier of the Linux kernel's eBPF: an abstract interpreter that decides whether a register-bytecode program is safe to run, by tracking a set of possible values for every register along every path.

## The idea

The kernel loads user programs and runs them with full privileges, so it must prove, before the first run, that a program cannot read or write out of bounds, use a value that was never set, or run forever. It does not run the program. It walks it.

- **Every register holds a description of its possible values.** A scalar has a `tnum` (a bitmask of the bits that are known and the value of those bits) and a range. The range is a `cnum` here, the circular number the current kernel uses in place of four separate unsigned and signed minimum and maximum fields. A pointer has a base (`mem` or `fp`) and an offset that is itself such a scalar.
- **The two descriptions refine each other.** After every operation the range is intersected with what the known bits allow, and the bits with what the range allows. A load of one byte gives `0..255` and bits `(0; 0xff)`, an `&` with 7 gives `0..7`, and a bit that is known to be zero tightens the range.
- **A branch is two paths.** At a conditional jump the verifier first asks whether the ranges already decide it. If they do, only one path is followed and the other is dead. If not, it follows one and pushes the other on a stack of pending states. On each path it narrows the registers by the condition: after `if r2 > 7 goto` the fall-through has `r2` in `0..7`, which is what makes `r1 + r2` a provably safe pointer.
- **A memory access is checked against the whole range.** The access is accepted only if the minimum and maximum offset both fall inside the buffer. Stack accesses must have a fixed, aligned offset and read only slots that were written.
- **State pruning keeps the walk finite.** At prune points (jump targets and instructions after a jump) the state is saved. If a later path arrives with a state that is covered by a saved one (each register range inside the saved range, each known bit agreeing), the path ends, since the saved one already proved everything below it safe.
- **Precision makes pruning effective.** Comparing every register would almost never match, so a register counts only if it mattered. After a path ends, the verifier walks back through the history of that path, and from each instruction that used a register as a pointer offset or in a branch it marks the registers that fed it as precise, through moves, arithmetic, stack spills and fills. Only precise registers are compared when a saved state is checked, and the others are treated as wildcards. This is the part that is least like any other analysis.
- **Loops are walked, not solved.** A loop is accepted if walking it terminates within the complexity limit with every pass proven safe. A state that repeats exactly on the same path is an infinite loop and is rejected.

The dumps in `dumps/<example>/stages.txt` show the program, its jump targets and prune points, then every instruction with the register state after it, each branch push, each prune with the saved state that covered it, and the precision marks propagated backward.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source. The short version: 64-bit scalars only (no `jmp32`), a memory pointer and a stack pointer, no linked-register ids, no liveness, no helpers, maps, subprograms, iterators or `may_goto`, and a complexity limit of 256 instead of one million. The kernel was read, not built or run. The oracle is a concrete interpreter in real Lua.
