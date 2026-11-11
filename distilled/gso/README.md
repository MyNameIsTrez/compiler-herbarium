# gso

A cut-down copy of the GNU superoptimizer (superopt 2.5): given a function, it finds the shortest instruction sequence that computes it, by trying every sequence.

## The idea

A superoptimizer does not translate a program. It is handed a function, the goal, and searches for the cheapest code for it. The goal is only an oracle that says what the output should be for an input. No IR is built from it and no rewrite rules are applied, so the sequences that come out are ones nobody would write by hand.

- **Iterative deepening.** Try every sequence of length 1, then every one of length 2, and so on. The first length with an answer is the shortest, so the result is optimal as far as the instruction set and the tests go.
- **A new register per instruction.** Instruction `n` writes register `arity + n` and reads inputs, constants or earlier results, so the search space is a tree of operand choices.
- **Prunes that never lose a shortest sequence.** An operation that reads the carry flag is not tried until something has set it. A commutative operation is tried with its operands in one order. The last instruction must consume the one before it, because otherwise that one was useless and a shorter sequence exists. Constants are not enumerated: each operation that takes one is listed by hand with the few constants that matter.
- **A cheap test first.** At a leaf the output is compared with the goal on one input, which rejects nearly everything. Only a sequence that matches goes on to the full set of test vectors, and a vector that rejects a sequence is moved to the front of the order, because it is likely to reject the next one.
- **No proof.** A sequence that passes the tests is trusted. GSO's README says it can be wrong with a very small probability, so this entry also runs the first sequence on 1000 held-out random vectors.
- **The carry flag is where the surprises are.** `(a == 0)` comes out as `sub.co #0, r0` then `add.ci r1, r0`: the first sets carry exactly when `a` is zero, and the second adds it to `-a + a`. The same shape as in the GSO paper. Unsigned `min` is four instructions, `sub.co`, `sub.ci r2, r2` (which turns the carry into a mask), `and`, `add`, the same four as Massalin's 68020 program. Signum takes three: `ashiftr r0, #31`, `add.co r0, #-1`, `add.ci r1, r1`, which is one of the sequences the real tool prints for it.

## What it runs on

Not the five programs in `examples/`. A superoptimizer's input is a function, so this entry is run on the kernels in [`kernels/`](../../kernels/): `./a.out <kernel>`. It prints, for every test vector, the output of the program it found, which must equal the kernel's `expected.txt`. The sequences found, with the search statistics, are in `dumps/<kernel>/stages.txt`. The entry generates no machine code: the sequences are in a toy instruction set that `run_program` interprets, as GSO's own test does.

## Checked against the real tool

The real superopt 2.5 was built with `gcc -std=gnu89 -O1 -w -DSPARC` and `-DPOWER` and run on the goals the kernels share. The lengths agree on every kernel (see the comparison in [FIDELITY.md](FIDELITY.md)). The sequence counts do not, because the real tool has a different instruction list per CPU.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: one toy target instead of fourteen, six goals instead of about a hundred, 22 or 52 test vectors instead of tens of thousands, one cost per instruction, and no multiplication, division, nullification or memory.
