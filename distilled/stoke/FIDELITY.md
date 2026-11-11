# Fidelity: stoke

Real design being mimicked: STOKE, the stochastic superoptimizer of Stanford (Schkufza, Sharma and Aiken, "Stochastic Superoptimization", ASPLOS 2013), as implemented in [StanfordPL/stoke](https://github.com/StanfordPL/stoke).

Reference material used (source tree, docs, papers). Pin every source repository to the full 40-character commit hash that was read, also when you name a tag or a branch, because tags and branches move:

- Source: [StanfordPL/stoke](https://github.com/StanfordPL/stoke) at commit `98d8a0f028f2daf2052bfe607dbc32ec8d55ba9e`, Apache-2.0, read through the GitHub API. Files read: `src/search/search.cc` (all of `Search::run`), `src/cost/correctness.cc` (`operator()`, `max_correctness`, `sum_correctness`, `evaluate_error`, `gp_error`, `rflags_error`, `undef_default`), `src/cost/latency.cc`, `src/transform/weighted.h`, `src/transform/instruction.cc`, `src/transform/opcode.cc`, `src/transform/local_swap.cc`, `src/transform/delete.cc`, `tools/args/weighted_transform.inc`, `tools/args/search.inc`, `tools/args/cost.inc` (the defaults), `tools/apps/stoke_search.cc` (the loop that calls `Search::run`, lines 418 to 552), `examples/tutorial/synthesize.conf`.
- Paper: the arXiv version of the ASPLOS paper (arxiv.org/abs/1211.0557), only the parts found by searching its text for restarts, chains, threads and parallelism: the design figure (Figure 9) and section 5.3 (parallel synthesis and optimization). The rest was not read.
- Not read: the rest of the ASPLOS paper, `src/transform/global_swap.cc`, `rotate.cc`, `operand.cc`, `add_nops.cc`, `opcode_width.cc`, `src/transform/pools.cc` (how operands are drawn), the validator (`src/validator/`), the sandbox, `src/cost/` beyond the two files above, and the tool's `synthesize` and `optimize` commands.
- Not built or run.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_goal` | test case generation | `CorrectnessCost` (`reference_out_`), the testcases given to the tool | The goal as an oracle over a fixed set of inputs | STOKE runs the target code in a sandbox to get the expected outputs, here the goal is a C function, and the inputs are the shared rule of `kernels/` |
| `stage_search` | the search | `Search::run` in `src/search/search.cc`, `WeightedTransform`, `CorrectnessCost`, `LatencyCost` | The walk itself: draw a move by mass, skip a move that is not valid, draw `p`, compute the bound `max`, call the cost function with `max + 1`, undo the move if the cost is above `max`, track `best_yet` and `best_correct`, stop at cost 0, and the per-move counts of proposed, valid and accepted. The cost function adds up the test inputs and stops once it reaches its limit. Moves: instruction, opcode, operand, swap. Beta 1.0, start from nops | See the deviations below |
| `stage_verify` | the final check | the validator, not read | Running the best program on the test inputs | STOKE proves equivalence with an SMT solver. Here the program is run on 1000 held-out random inputs |

## Deliberate deviations

- Toy instruction set instead of x86-64: two-address operations on four 32-bit registers and a carry flag, with x86 flag behavior for the operations that have one. The inputs are in `r0` and `r1`, the other registers and the flag start at zero, and `r0` is the output. STOKE rejects a program that reads an undefined register, and this entry defines them as zero instead.
- The correctness cost is the Hamming distance of `r0` to the expected value, summed over the test inputs, like the default `Distance::HAMMING` with `Reduction::SUM`. The real cost compares every live-out register and flag, and lets a value in the wrong register count with a penalty (`relax_reg`, `misalign_penalty`). A program that crashes also costs a penalty (`sig_penalty`). None of these exist here.
- The performance term is one per instruction. The real default cost is `correctness+measured`, with a measured runtime, and `LatencyCost` is a table of Haswell latencies.
- The two phases are two runs of the same loop with different start and cost. STOKE's `init` defaults to `ZERO` and `initial_instruction_number` to 16, here the program has six slots. The real tool has `synthesize` and `optimize` commands that I did not read.
- Move masses follow `weighted_transform.inc`: instruction, opcode, operand, local swap and global swap are 1 each, and delete, add-nops and opcode-width are 0. Local and global swap are the same move on a one-block program, so `swap` has mass 2. `rotate` is also 1 by default and is left out, because I did not read it.
- The operand move picks a random operand and a random new value, where STOKE draws from pools of operands that are valid at that point (`pools.cc`, not read). The opcode move picks any opcode of the same shape, including the current one.
- There is no logarithm: `thr[d]` is `2^32 * exp(-d)` in fixed point, and the bound is the largest `d` for which the random `p` is below `thr[d]`. This is the same bound as `max = cost - log(p) / beta`, without libm. It is rounded, and capped at an increase of 23.
- Restarts and several chains. `Search::run` is a single chain, and the real `search` tool (`stoke_search.cc`) wraps it in a loop: each cycle starts from a fresh state, runs for an iteration limit given by `--cycle_timeout` (a list of expressions in the cycle number, so the limit can grow), checks the result with the verifier, and restarts if there was no correct rewrite or it did not verify, optionally adding the verifier's counterexample as a test case. It stops at the first verified result. This entry has the restart on failure (a fixed cap of 400000 iterations, no growth, no counterexamples) but then runs four chains to the end and keeps the shortest. That part is closer to the paper, whose section 5.3 runs synthesis and optimization in parallel on a cluster for a fixed time, and the design figure says the results are ranked and the lowest cost is returned. The real tool does not do it in one process, and the paper's ranking is by cost and then measured runtime, here it is by length only.
- The test inputs are fixed, where STOKE takes random ones and adds the counterexamples its validator finds. No validator runs here.
- No signal handler or timeout in seconds: the iteration cap is the only limit.

## Comparison with the real tool

None. The real tool was not built or run. The results were compared only with `distilled/gso`, whose lengths are the optimum for a different instruction set, so they say how far the walk is from the optimum and nothing about the real STOKE.

## Regenerating expected.txt

Every `examples/<kernel>/expected.txt` is generated by compiling the kernel's `goal.txt` expression with gcc and evaluating it on the test vectors, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
scripts/gen-kernel.sh distilled/stoke/examples/*/
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
