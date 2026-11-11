# Fidelity: gso

Real design being mimicked: the GNU superoptimizer, `superopt` 2.5 (1995), the program behind Granlund and Kenner's "Eliminating Branches using a Superoptimizer and the GNU C Compiler" (PLDI 1992). The search idea comes from Massalin's "Superoptimizer: A Look at the Smallest Program" (ASPLOS 1987), whose code was not read.

Reference material used (source tree, docs, papers). Pin every source repository to the full 40-character commit hash that was read, also when you name a tag or a branch, because tags and branches move:

- Source: [superopt-2.5.tar.gz](https://ftp.gnu.org/pub/gnu/superopt/superopt-2.5.tar.gz), not a git repository, sha256 `4e28eb00a835730b8f33487c78094a6ced2c4e7cbcd37937ac6081b9b59925d7`. Files read: `README`, `superopt.c` (`recurse`, `recurse_last`, `init_test_sets`, `test_sequence`, `main_synth`, the operand names), `synth.def` (the head and the generic-target block up to the shifts), `goal.def` (the head), `superopt.h` (`PERFORM_*` macros for the carry operations, `enum prune_flags`, `TRUNC_CNT`).
- Papers: Granlund and Kenner (the first 10 pages) and Massalin (all 5 pages).
- Not read: `run_program.def`, `insn.def`, the per-target blocks of `synth.def` (the CISC and nullifying variants), the rest of `goal.def`, `longlong.h`, `TODO`, `ChangeLog`.
- Built and run: `gcc -std=gnu89 -O1 -w -DSPARC -o superopt-SPARC superopt.c` and the same with `-DPOWER`, run as `./superopt-SPARC -f<goal> -max-cost 4` (the output is in generic operation names, since `-assembly` was not passed). The binary has no generic target, so the comparison below is against SPARC and POWER.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_goal` | goal selection and `init_test_sets` | `goal.def` (`DEF_GOAL`), `init_test_sets` in `superopt.c` | The goal as a line of C, the test set made of every combination of a few edge values followed by pseudo-random vectors | Six goals instead of about a hundred, chosen on the command line in the real tool with `-f`. Six edge values instead of sixteen and 16 random vectors instead of 25000 |
| `stage_search` | `main_synth`, `synth`, `recurse`, `recurse_last`, `test_sequence` | `main_synth`, `recurse`, `recurse_last` and `test_sequence` in `superopt.c`, `synth` in `synth.def` | Iterative deepening on the cost, the first test with a goal value picked to be non-zero, a new register per instruction, the carry state with `CY_JUST_SET`, the last instruction consuming the previous result, one operand order for commutative operations, constants listed by hand, the adaptive reordering of test vectors, printing every sequence of the shortest length as it is found | Every instruction costs 1, so there is no `-max-cost`, `-extra-cost`, CISC cost or nullification. The search never gives up, because every kernel here has an answer. Only generic operations, no multiplication, division, extracts, byte or half-word operations |
| `stage_verify` | the end of `test_sequence` | `test_sequence` in `superopt.c` | Running a found sequence on the test inputs | Nothing here prints assembly. The held-out check is added, since GSO's README warns that a sequence can be wrong. The stdout lines are this repo's contract |

## Deliberate deviations

- One toy target instead of fourteen: the operations are GSO's generic ones with the semantics of the `PERFORM_*` macros in `superopt.h` (`add.co` is `ADD_CO`, `sub.co` is `ADC_CO`, `add.ci` is `ADD_CI`, `sub.ci` is `ADC_CI`, `andc` and `nor` as there), so the carry is set by `sub.co` when there is no borrow, like the RS/6000 `sf`. No `CIO` variants, which both read and set the carry.
- The forms with a constant or a repeated register are my own pick of thirteen, in the style of the lists in `synth.def`, not a copy of them. The real lists differ per target.
- The shift counts are 1 and 31, the real defaults (`cnt += BITS_PER_WORD - 2`), but arithmetic shift is only tried by 31.
- The real tool picks random values with a non-zero goal for the first test, up to 50 tries. This entry takes the first test vector whose output is not zero.
- The real `test_sequence` swaps a failing test set with the one at half its index in the test array. This entry keeps a separate order array and swaps there, so the stdout lines keep their order.
- The prune that the last instruction must consume the previous result checks both operands in the real code. The loops here put the larger register first, so only that one can be the previous result.
- A sequence is accepted when it passes the tests and is never proved equal, as in the real tool. The 1000 held-out vectors are an addition.
- The first sequence found is the one run for stdout. The real tool has no such output.

## Comparison with the real tool

Shortest length reached by the real tool on the same goals (`-max-cost 4`, or 5 for `sgn`), and the number of sequences it printed at that length, against this entry:

| Kernel | Real, SPARC | Real, POWER | This entry |
|--------|-------------|-------------|------------|
| `abs` | 3 (4 sequences) | 1 (it has an `abs` instruction) | 3 (4 sequences) |
| `eq0` | 2 (8 sequences) | 2 (1 sequence) | 2 (1 sequence) |
| `ges0` | 2 (6 sequences) | 2 (6 sequences) | 2 (2 sequences) |
| `maxu` | 4 (28 sequences) | 4 (14 sequences) | 4 (24 sequences) |
| `minu` | 4 (28 sequences) | 4 (18 sequences) | 4 (42 sequences) |
| `sign` (`sgn`) | 3 (13 sequences) | 3 (6 sequences) | 3 (2 sequences) |

- The lengths agree wherever the real target has no extra instruction that gives it a shortcut. POWER's `abs`, `clz` and carry-setting shift are not in this entry.
- The sequence counts differ, as expected: every real CPU has its own operation list, with `CIO` operations, SPARC's borrow-style `sub_co`, and many more constant forms. The shapes agree. For example the real `eq0` on POWER is `adc_co(0,r0)` then `add_cio(r1,r0)`, and this entry's is `sub.co #0, r0` then `add.ci r1, r0`.
- The first version of this entry found `sign` in 4, because its constant forms had no `add.co x, #-1`. The real tool's SPARC output showed a length-3 sequence using it. Adding that one form made this entry find the same sequence (`ashiftr r0,#31; add.co r0,#-1; add.ci r1,r1`).
- The sequences were also compared by hand with the GSO paper (`(a == 0)` in two instructions with `sfi` and `ae`, absolute value in three) and with Massalin's (unsigned `min` in four).
- Not compared: the number of instructions the real search tried, its timing, and its output with `-assembly`.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
