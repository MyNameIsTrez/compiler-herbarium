# Fidelity: tcc

Real design being mimicked: the Tiny C Compiler (TinyCC), its x86-64 code generator.

Reference material used:

- Source: [TinyCC/tinycc](https://github.com/TinyCC/tinycc) at `aade7bafb59641660bc9a0e21ad0a9a4a1ed346e` (HEAD of `mob` when cloned, shallow). Files read: `tccgen.c` (`gsym`, `vsetc`, `vset_VT_CMP`, `vset_VT_JMP`, `gvtst_set`, `gvtst`, `save_reg_upstack`, `get_reg`, `gv`, `gv2`, the start of `gen_op`, `expr_infix`, `expr_landor`, `block` for `if`, `while`, `for`, compound statements and `return`, `check_func_return`, the start of `vstore` and `gen_function`), `x86_64-gen.c` (`gsym_addr`, `gjmp`, `gjmp_addr`, `gjmp_append`, `gjmp_cond`, `gen_opi`, the parts of `load` for constants, locals, registers and flags, the SysV `gfunc_call`, `gfunc_prolog`, `gfunc_epilog`), and the value location defines and `SValue` in `tcc.h`.
- Not read: `tccpp.c` (the preprocessor and tokenizer), `unary` and the type machinery in `tccgen.c`, `gen_opic` (constant folding), `store`, `tccelf.c` (sections and relocations), the debug and bounds-checking code.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | Preprocessing and tokenizing | `tccpp.c` (`next`) | The token numbering: single characters stand for themselves, `TOK_LT` is the condition code | The tokenizer and preprocessor: each example's tokens are written down by hand |
| `stage_codegen` | Parsing and code generation in one pass | `tccgen.c` (`unary`, `expr_infix`, `block`, `gen_function`, `gv`, `gen_op`, `vstore`, `gvtst`, `gsym`), `x86_64-gen.c` (`load`, `gen_opi`, `gjmp_cond`, `gsym_addr`, `gfunc_call`, `gfunc_prolog`, `gfunc_epilog`) | The value stack with locations and flags, lazy loading by `gv`, immediate forms, flags consumed by `gvtst`, jump chains threaded through rel32 fields, the prologue patched after the body, the implicit `return 0` of `main`, the jump after `return` that is left out for the last statement | See below |
| `stage_export` | Linking and loading | `tccelf.c`, `tccrun.c` | The bytes of the program in one buffer, run from `main` | Sections, symbols and relocations. TCC puts strings in `.rodata`, relocated at link time. Here they follow the code, and their references are a jump-style chain resolved at the end |

## Deliberate deviations

- The tokenizer is replaced by hand-written token arrays, and there are no error messages: the tokens are well formed, so `skip` never checks.
- The language is the subset the examples use: `int` only, functions with `void` or one `int` parameter, `int x = e;`, assignment, `if`/`else`, `while`, `return e;`, blocks, calls with exactly one argument, string literals as call arguments, and `+ - * <`. There are no other types, pointers, arrays, unary operators, loops other than `while`, or declarations without an initializer.
- No constant folding. TCC's `gen_opic` folds `2 + 3` to `5` before any code exists, which every compiler does, so it is left out. The code for `(2 + 3) * 4 - 6` therefore shows the immediate forms and the lazy load of constants that TCC uses when only one operand is constant.
- Registers are never spilled. TCC's `save_reg` and `gfunc_call`'s `save_regs` store live registers to temporary locals, and `get_reg` frees one when none is left. At most two values are in registers at a time here, and none lives across a call, because a local is only loaded when an operation needs it.
- Flags are only used as a condition. TCC can also turn them into 0 or 1 in a register (`VT_CMP` in `load`), chains of `&&` and `||` (`VT_JMP`, `VT_JMPI`, `gjmp_append`, `expr_landor`), and `gvtst` handles both the true and the false jump. Here a comparison is always the condition of an `if` or `while`, and the jump is taken when it is false.
- `gv` asks for any integer register, and `gfunc_return` moves the value to rax afterwards. TCC has register classes, so `gv(RC_IRET)` loads straight into rax. The arguments are also moved into rdi after `gv`, as in `gfunc_call`.
- `expr_infix` always recurses for the right operand. TCC skips the call when the next operator does not bind tighter, which gives the same result.
- Only the `<` condition exists, so the inverse code is `op ^ 1`, as in `gvtst`, with `jge` as the one jump.
- The jump after `return` is skipped only for a `return` at the top level of the function body that is followed by `}`. TCC also jumps when another statement follows it there, which no example has.
- Offsets in `rbp` addressing fit in 8 bits, backward jumps are short, and constants are immediates of 8 bits. TCC chooses between the short and the 32-bit forms.
- A parameter is stored to a local with a 64-bit `mov` and read back with a 32-bit one, as in `gfunc_prolog`. Locals are 4-byte aligned from the frame pointer down.
- Functions are placed before they are called, so a call is a known rel32. TCC emits a relocation for every call and resolves it at link time.
- `print_int` and `print_str` are hand-assembled `write` system calls placed first in the code, standing in for libc.

## Comparison with the real tool

The real TCC was not built or run for this entry, so no output was compared.

- Matches: the instruction forms (`mov eax, [rbp-8]`, `cmp eax, imm8`, `imul`, `jge rel32`, `call rel32`), the registers picked in order (rax, then rcx), the arguments moved into rdi, the `leave; ret` epilogue and the prologue patched after the body are what the source emits, as read from `x86_64-gen.c`.
- Differs: see the deviations. Real TCC also emits relocations and an ELF object, and folds constants.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
