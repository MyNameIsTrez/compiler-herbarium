# Fidelity: ebpf-verifier

Real design being mimicked: the verifier of the Linux kernel's eBPF (`kernel/bpf/`), the abstract interpreter that proves a program safe before it is allowed to run.

Reference material used:

- Source: [torvalds/linux](https://github.com/torvalds/linux) at `067c5c24cef23d913b67eaaf71ae67e7169a6620` (shallow sparse clone). Files read: `kernel/bpf/tnum.c`, `kernel/bpf/cnum.c`, `kernel/bpf/cnum_defs.h`, `include/linux/cnum.h`; in `kernel/bpf/verifier.c` the bounds updates (`__update_reg32_bounds`, `__update_reg64_bounds`, `__reg_bound_offset`, `reg_bounds_sync`), the scalar ALU (`scalar_min_max_add`, `sub`, `mul`, `and`, `or`, `xor`, `lsh`, `rsh`, `adjust_scalar_min_max_vals`), the branch logic (`is_scalar_branch_taken`, `is_branch_taken`, `regs_refine_cond_op`, `bpf_rev_opcode`, `check_cond_jmp_op`), memory checks (`__check_mem_access`, `check_mem_region_access`), `push_stack` and `do_check`; in `kernel/bpf/states.c` `range_within`, `regsafe`, `states_equal`, `propagate_precision`, `bpf_is_state_visited`, `bpf_split_cur_state`, `bpf_update_branch_counts`; in `kernel/bpf/backtrack.c` `bpf_push_jmp_history`, `get_prev_insn_idx`, `get_jmp_hist_entry`, `backtrack_insn` (ALU, LDX, STX and jump cases) and `bpf_mark_chain_precision`.
- Not read: `cfg.c` beyond a search of its function names, `liveness.c`, `loops.c`, `scev.c`, 64-bit division and modulo, the checks for every other pointer type, helper calls, subprograms, iterators, the BTF code, and the speculative execution sanitizer.
- Built and run: nothing. The kernel was not built.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | the loaded program | the `bpf()` syscall and the instruction array (not read) | A program as a list of instructions in the kernel's assembly notation, with a buffer pointer in `r1` and a frame pointer in `r10` | No syscall, no ELF, no maps, no BTF. An example gives the instructions directly |
| `stage_cfg` | `check_cfg` and the prune point marking | `cfg.c` (names only), `mark_prune_point` and `mark_jmp_point` in `verifier.c` | Jump targets and the instruction after a conditional jump are prune points | No depth-first search for unreachable code or for back edges, no strongly connected components. Reachability is found by walking |
| `stage_verify` | `do_check` | `verifier.c`, `states.c`, `backtrack.c`, `tnum.c`, `cnum.c` | A register is a tnum plus a cnum range or a pointer with a variable offset. After every operation the range and the bits are made to agree. A conditional jump asks `is_branch_taken` first, and otherwise pushes the other path and narrows the registers on both sides by the condition. Memory accesses check the minimum and the maximum offset. Stack accesses are fixed, aligned and read only after a write. At a prune point the state is saved, and a later state that is covered by a saved one ends its path (`regsafe`, `states_equal`). Precision is marked by walking the history of the path backward (`backtrack_insn`, `bpf_mark_chain_precision`), and only precise registers and stack slots are compared. A state that repeats on the same path is an infinite loop, and the walk stops at a complexity limit | See the deviations below |

## Deliberate deviations

- Only 64-bit operations. There is no `jmp32`, no 32-bit ALU, and no `var_off` subregister bounds, so `__update_reg32_bounds` and the 32-bit halves of `reg_bounds_sync` are not mirrored.
- The kernel's current verifier keeps a scalar's range as a `cnum`, a circular number, in place of the older `umin`, `umax`, `smin` and `smax` fields. This entry uses the cnum only, written branchless with a select helper. Its operations were read in `cnum.c` and `cnum_defs.h`, and the entry's are a reduced set, not line-by-line copies.
- No linked scalars: the kernel gives registers copied from one another a shared id so that narrowing one narrows the others. There is no id here.
- No liveness: the kernel cleans registers and slots that are never read again from a saved state so that more states match. This entry compares only by precision.
- No `tnum_step` and no stride tracking.
- Every prune point is saved and nothing is evicted. The kernel decides whether to save a state by heuristics (the number of instructions processed since the last save, the number of branches) and frees old ones.
- The complexity limit is 256 instructions, where the kernel's is one million, so that an example of a too-complex program stays small.
- Jump normalization: the eleven jump operators are turned into four (equal, unsigned less, signed less, bit set) with a swap and a negation, and the narrowing is written once. The kernel writes each case in `regs_refine_cond_op` and its helpers.
- The history holds one entry for every executed instruction. The kernel pushes an entry only at jumps and stack accesses and finds the others by stepping back through the instruction array.
- When a pointer gets a register added to it, the register is marked precise at that instruction. Where the kernel marks it was not verified.
- No helpers, maps, null checks, subprograms, iterators, `may_goto`, other pointer types (context, packet, map values, BTF) or speculative sanitization. There is one memory pointer with a size and one stack of eight slots.
- The oracle is a concrete interpreter that samples a wider load at two extremes. It checks that the program is actually safe, which is not the property the verifier proves, and it does not know the kernel's reasons for rejecting.
- The dump notation (`R1=mem+0`, `state saved`, `covered by the state saved earlier`, `is precise in the state saved at`) imitates the kernel's log but is this entry's own.

## Comparison with the real tool

None run: the kernel is not built here. The oracle is a concrete interpreter in real Lua 5.4. The shape of the register state, the narrowing at a branch, the pruning test and the backward precision walk were compared with the files listed above.

## Regenerating expected.txt

The oracle is `oracle.lua`, run by real Lua 5.4. Each `program.lua` loads it and calls `run` with the buffer size and the program text, which `main.c` also holds in its `examples` table. The two must agree.

```
for d in distilled/ebpf-verifier/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
