# Fidelity: puc-lua

Real design being mimicked: the reference Lua 5.4 virtual machine, PUC-Rio Lua.

Reference material used:

- Source: [lua/lua](https://github.com/lua/lua) at tag `v5.4.7` (commit `1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3`): `lopcodes.h` (instruction layout and opcode numbers), `lvm.c` (`luaV_execute`, `pushclosure`), `ldo.c` (`luaD_poscall`), `lfunc.c` (`luaF_close`), `ltm.c` (`luaT_adjustvarargs`), `lcode.c` (`luaK_finish`).
- Output of `luac5.4 -l -l -p` and `string.dump` from the installed Lua 5.4.9, used to copy the bytecode. That repository checkout contains no `luac.c`, so the listing format is modeled on the printed output, not on its source.
- Not read: `lparser.c` and `lcode.c` in depth, which are the compiler that this entry skips, and the garbage collector (`lgc.c`).

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_select` | Lexing, parsing and code generation | `llex.c`, `lparser.c` (`luaY_parser`), `lcode.c` | The result: a `Proto` per function, with instruction words, constants and upvalue names, in the real 5.4 encoding | All of the compiler. The prototypes are copied from `luac5.4 -l -l`, and the instruction words were checked to occur in `string.dump` output. Line info, local variable names and `numparams`/`maxstacksize` are not kept |
| `stage_list` | Bytecode listing | `luac -l -l` | Instruction numbers from 1, mnemonics, operands, `; to N` jump targets, constant and upvalue comments | Line numbers, the `k` flag on operands other than RETURN, the "N in M out" comments, the constants and upvalues tables. The raw hex word is added |
| `stage_run` | Execution | `lvm.c`: `luaV_execute` | One `switch` on the opcode, a register window `base` into one stack, call frames, `top` for variable-length lists, the instruction-skipping convention (arithmetic skips its `MMBIN`, comparisons skip or take the `JMP` after them), `luaD_poscall`, open upvalues that `RETURN` with `k` closes | See below |

## Deliberate deviations

- Only the opcodes the examples execute have handlers: `MOVE`, `LOADI`, `LOADK`, `GETUPVAL`, `GETTABUP`, `ADDI`, `ADD`, `MUL`, `JMP`, `LT`, `LTI`, `CLOSURE`, `CALL`, `RETURN`, `RETURN1`, `VARARGPREP`. `RETURN0`, `MMBIN` and `MMBINI` appear in the bytecode but never run, so they have no handler. Code that cannot run cannot be covered.
- Values are integers, strings, one table, closures and one C function. There are no floats, no metatables and no garbage collector, so arithmetic on anything but integers aborts where the real handlers fall through to `MMBIN`/`MMBINI`.
- `_ENV` is a table with the single key `print`. `GETTABUP` compares that key and aborts on any other.
- `VARARGPREP` does nothing. The real one (`luaT_adjustvarargs`) copies the function and its fixed parameters above the extra arguments. A main chunk has no fixed parameters and gets no extra arguments here, so there is nothing to move.
- `RETURN` is only the main chunk's return: it closes upvalues if `k` is set and ends the run. The real handler also hands `n` results to `luaD_poscall`. A function returns its result with `RETURN1`, which here shares `poscall` instead of having the real handler's inlined fast path.
- `poscall` does not pad missing results with nil.
- `CLOSURE` creates one upvalue per closure that always captures a register of the running function. The real `pushclosure` also looks for an existing open upvalue to share, and can capture the enclosing closure's upvalues.
- The frame keeps `pc` and `base` in a struct that is re-read on every instruction. The real loop caches both in locals, and reloads them at `startfunc` and `ret`.
- Stack overflow, the C stack limit, hooks and error handling are absent.

## Comparison with the real tool

- Command: `luac5.4 -l -l -p distilled/puc-lua/examples/<example>/program.lua`
- Matches: for all five examples the instruction words equal those in `string.dump` of the real program, and the mnemonics and operands in `stage_list` equal the real listing. The trace in `stage_run` shows the same executed instruction sequence a real run follows, including `RETURN1` in the recursive `fact` and `RETURN` with `k` closing the upvalue of `fact` in the main chunk.
- Differs: see the deviations. Real `luac` also prints line numbers and tables of constants, locals and upvalues.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/puc-lua/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
