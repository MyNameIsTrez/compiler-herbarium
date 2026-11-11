# stoke

A cut-down copy of STOKE, the stochastic superoptimizer: given a function, it finds fast code for it by a random walk over whole programs, scored on test inputs.

## The idea

GSO ([`distilled/gso`](../gso/)) tries every sequence, so it can only reach a few instructions. STOKE gives up that guarantee and samples instead. It keeps one complete program and keeps changing it a little, and it keeps a change when the program is no worse, or, with a probability that falls quickly, even when it is worse. That is the Metropolis-Hastings algorithm.

- **The state is a whole program.** Here it is six slots, each an instruction or a `nop`, so the search can both grow and shrink a program by moving `nop`s in and out.
- **Four kinds of move.** A new opcode of the same shape, a new operand, a whole new instruction, or two instructions swapped. Each has a weight.
- **Accept with `max = cost - log(p) / beta`.** Draw a random `p` in (0, 1). A proposal whose cost is at most `max` is kept. With `beta = 1`, a proposal that is 1 worse is kept about 37% of the time and one that is 5 worse about 0.7%. The walk climbs out of local minima, but only slowly.
- **The cost has a slope.** It is the number of wrong bits over all test inputs, so a program that is almost right is cheaper than one that is far off, and the walk can follow that. A count of right and wrong tests would be flat nearly everywhere.
- **The cost function stops early.** It is handed the largest cost that would still be accepted, and stops adding up test inputs once it passes it. Most proposals are rejected after a few inputs.
- **Two searches with two costs.** Synthesis starts from all `nop`s and counts only wrong bits, until some program is right. Optimization starts from that program and adds one per instruction, so the walk is paid for deleting instructions and moves toward shorter ones while it stays right.
- **No proof, and no guarantee.** A program is right when it passes the tests, and the walk can stop at a longer program than the best one.

## What it runs on

The same kernels as `gso`, copied into [`examples/`](examples/): `./a.out <kernel>` prints, for every test vector, the output of the shortest correct program it found. The search it took to get there is in `dumps/<kernel>/stages.txt`: how many iterations each chain needed, how many cost evaluations stopped early, and for each move how many proposals were made, valid and accepted.

The results of this entry and of the exhaustive search in `gso`, on the same toy ideas but not the same instruction set (this one is two-address with four registers and overwrites them, `gso` writes a new register each time):

| Kernel | `gso` (the shortest there is) | `stoke` (the shortest it found) |
|--------|------------------------------|---------------------------------|
| `abs` | 3 | 4 |
| `eq0` | 2 | 3 |
| `ges0` | 2 | 2 |
| `maxu` | 4 | 4 |
| `minu` | 4 | 4 |
| `sign` | 3 | 4 |

The stochastic search matches the optimum on three of six kernels and is one instruction off on the other three. It also takes about two seconds where `gso` takes a fraction of one, because at this size exhaustive search wins. Random search pays off when the sequences are too long to enumerate, which none of these kernels are.

A chain that finds nothing within its cap is thrown away and another starts from `nop`s, as the real `search` tool does when a cycle times out without a verified result. The entry also runs four chains to the end and keeps the shortest result, which is closer to the paper, where many threads search in parallel and the best result is returned.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: the search loop, the acceptance rule, the early-stopping cost function, the move statistics and the default move weights follow the real code. The x86 instruction set, the sandbox that runs candidate code, the performance model and the validator are replaced or left out.
