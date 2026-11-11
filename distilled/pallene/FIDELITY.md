# Fidelity: pallene

Real design being mimicked: Pallene, the compiler from a typed subset of Lua to C, in particular the command IR, the garbage collection pass and the C code generator.

Reference material used:

- Source: [pallene-lang/pallene](https://github.com/pallene-lang/pallene) (MIT) at `b814ba20021a154be339896c94ade59dfcdcd1e5` (the head of `master` when it was read, downloaded file by file with no clone). Files read in full: `src/pallene/ir.lua`, `gc.lua`, `flow.lua`, `uninitialized.lua`, `builtins.lua`. Read in part: `types.lua` (the type union and `is_gc`), `assignment_conversion.lua` (the header comment), `typechecker.lua` (the header comment and the tail of `check_exp_verify` where an `ast.Exp.Cast` is made), `to_ir.lua` (`BlockBuilder`, `convert_func`, `exit_function`, `insert_return`, the statements `While`, `If`, `Decl`, `Assign`, `Call`, `Return`, `Break`, the start of `ForNum`, `exp_to_assignment` for `InitList`, `Concat`, `Cast` and the builtin calls, `value_is_truthy`, the start of `exp_to_value`), `coder.lua` (the section list, `pallene_entry_point_definition`, `update_stack_top`, `generate_blocks`, `generate_cmd`, and the generators of `ForPrep`, `Jmp`, `JmpIf`, `CheckGC`, `Concat`, `ToDyn`, `CallStatic`, `CallDyn`, `BuiltinIoWrite`, `BuiltinTostring`, `c_var`, `call_pallene_function`), `driver.lua` (only the order of the passes, by search).
- Source: [lua/lua](https://github.com/lua/lua) at `8cb93bada561977eb370819e39d7d644e5fd0a3f`. Read: `traversethread` in `lgc.c`, for what the collector scans and what it clears.
- Not read: `lexer.lua`, `parser.lua`, the body of `typechecker.lua`, the rest of `assignment_conversion.lua` and of `to_ir.lua`, `constant_propagation.lua`, `pallenelib.lua`, the record, array and table generators and the runtime in `coder.lua`, the C runtime (`pallene_core.h` and `pallene_core.c`, where `pallene_string_concatN`, `pallene_tostring` and `pallene_io_write` live), `pallenec.lua`, and all of Lua's runtime apart from `traversethread`.
- Pallene was not built or run. Everything below is from reading the Lua source.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | Lexer and parser | `Lexer.lua`, `parser.lua`, the tree in `ast.lua` | A typed program as a tree: declarations with a type annotation, `if`, `while`, `return`, calls, `..` | The whole front end, since the trees are built by hand. Only the constructs the examples use |
| `stage_typecheck` | Typechecker | `typechecker.lua` (`check_exp_verify`, which wraps an expression in `ast.Exp.Cast` when its type is consistent with, but not equal to, the expected one) | Every expression gets a type, and a cast is inserted where the expected type differs. The builtins `tostring` (any to string) and `io.write` (string to nothing) have the signatures of `builtins.lua` | Type errors, scoping and shadowing, subtyping, all types but integer, boolean, string and any, and the other builtins. A cast is only ever a conversion to `any` |
| `stage_to_ir` | `to_ir` | `to_ir.lua` (`ToIR:convert_stat`, `exp_to_assignment`, `exp_to_value`, `BlockBuilder:finish_block`, `insert_return`), the commands in `ir.lua` | A list of basic blocks per function of flat commands, one temporary per subexpression, structured control flow lowered to `Jmp` and `JmpIf` with the same block layout as the real code (an `if` without `else` leaves a block that is never reached, as in `fact`), `return` as a move into the return variable and a jump patched to the last block, `CheckGC` after `Concat` | Floats, for loops, arrays, tables, records, closures and upvalues, `break`, multiple returns, dynamic calls, `FromDyn`, the other builtins and locations. Constant folding of `JmpIf` (`ir.clean`) |
| `stage_gc` | `gc.compute_gc_info`, with `flow.flow_analysis` | `gc.lua` (`compute_stack_slots`), `flow.lua` | A backward live-variable analysis over collected variables only, the live set before each `CallStatic` and `CheckGC`, the table of which variables are alive at the same time, and greedy slot assignment with the lowest free slot | `compute_vars_to_mirror` (reaching definitions of collected variables at collection points): the real `gc.lua` computes it, but nothing in `coder.lua` reads it, so it is left out. The generic gen/kill framework: the transfer of a block is applied command by command. See the deviations |
| `stage_coder` | `Coder:init_gc` and the generators of `coder.lua` | `coder.lua` (`pallene_entry_point_definition`, `generate_cmd`, `update_stack_top`, `generate_blocks`) | After every command that defines a variable with a slot, a store to `s2v(base + slot)`; before a collection point, `L->top.p = base + <highest live slot + 1>`; the frame check from the frame size; `goto` to a label per block, with a jump to the next block left out; C locals named `x<id>`; functions named `function_<id>` | The generators of the commands the examples do not use, `savestack` and `restorestack` after a call, `PALLENE_SETLINE` and frame tracking, `max_lua_call_stack_usage` in the stack check, the Lua entry points that wrap each function, the module and `luaopen` functions, upvalues. Strings are printed as C literals and not as upvalue slots |
| `stage_run` | The C compiler, the C runtime and Lua's collector | not part of Pallene; `lgc.c` (`traversethread`) for the scan | The collector marks the stack from the bottom to the top and nothing else, frees what is not marked, and clears the dead part of the stack above the top, as `traversethread` does in its atomic step. A callee's frame starts where the caller's stack top was | Everything about the real collector but the stack scan: incremental and generational modes, the debt that makes `luaC_condGC` collect only now and then, write barriers, finalizers. See the deviations |

## Deliberate deviations

- The examples are typed Pallene programs written by hand, not copied from anywhere. Pallene has no `print` at the pinned commit, only `io.write` of one string, so each example builds its text with `tostring` and `..` and writes it. `hello` splits the text into several string variables so that two of them are alive at once. None of this is checked against what Pallene's parser and typechecker would produce.
- The real `init_start` in `compute_stack_slots` marks every return variable live in the exit block, and `gen`/`kill` only touch collected variables, so as read a return variable of any type stays live in all blocks of its function, and an integer return variable such as `fact`'s would get a Lua stack slot and a store after every assignment. This entry marks only a return variable of a collected type. This is a reading of the code, not something observed by running Pallene, and it may be a mistake in the real pass.
- The real code assigns slots by iterating a Lua table with `pairs`, so its order is not defined. This entry goes through the variables in the order they were declared, which makes the dumps stable.
- Live sets are 32-bit masks, so a function has at most 16 variables here. The real sets are Lua tables and a block's effect is built from the commands' gen and kill sets, then iterated to a fixed point over the blocks in an order from a depth-first search. This entry iterates the commands directly, in a reverse postorder of the reversed graph from the exit, which reaches the same fixed point.
- The entry executes the annotated IR on an interpreter. Pallene writes C and a C compiler builds it into a Lua module, which this entry cannot do. `stage_coder` still prints the C it would write, and what runs is the same annotated commands, so the stack stores and the stack top that run are the ones printed.
- The toy collector runs at every `CheckGC` (the stress mode of Lua's own collector). The real `luaC_condGC` collects only when the allocation debt says so, so on a small program the real collector may never run, and a missing mirror write would stay hidden.
- The toy stack is untagged: a slot is the number of a heap object, and every slot below the top is one, because only collected variables get a slot and the examples have only strings. The real stack holds tagged values, and an `any` variable holding an integer would be one. String literals are not interned, each use makes a new permanent object, and there is no stack growth, so the frame check is only printed.
- Collected parameters are not modelled. The real calling convention has the caller save them in the callee's frame, so a callee can assume they are on the stack. No example passes a string.

## Comparison with the real tool

None. Pallene needs Lua with its development headers and a C compiler, and was not built or run. The shape of the C that the entry prints (a store to `s2v(base + n)` after an assignment, `L->top.p` set before a collection point, one label per block) was compared by reading `generate_cmd`, `update_stack_top` and `generate_blocks`. Pallene's own dumps (it can stop after a pass, see `driver.lua`) would be the thing to compare with `stage_to_ir` and `stage_gc`.

- Command: none run.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/pallene/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
