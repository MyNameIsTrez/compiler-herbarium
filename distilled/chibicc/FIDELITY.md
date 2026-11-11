# Fidelity: chibicc

Real design being mimicked: [rui314/chibicc](https://github.com/rui314/chibicc), a small C11 compiler for x86-64, at commit `90d1f7f199cc55b13c7fdb5839d1409806633fdb` (a shallow clone of the default branch).

Reference material used:

- Source: the repository above.
- Read in depth: `codegen.c` for everything the examples exercise: `push`, `pop`, `count`, `gen_addr`, `load`, `store`, `cmp_zero`, `gen_expr` (numbers, variables, assignment, `ND_COMMA`, `ND_MEMZERO`, function calls with `push_args` and `push_args2`, binary operations), `gen_stmt` (`if`, `for`, `return`, blocks), `assign_lvar_offsets`, `emit_text` and `codegen`. In `parse.c`: `function` (the parameters, `__alloca_size__` and the two `__func__` strings), `create_param_lvars`, `funcall`, `new_lvar`, `new_gvar`, `new_unique_name`, `new_string_literal`. In `type.c`: `add_type`.
- Skimmed only: the lines of `parse.c` that build `ND_MEMZERO` for an initializer (line 1384).
- Not read: `tokenize.c`, `preprocess.c`, `main.c` (the driver that runs `as` and `ld`), `unicode.c`, `hashmap.c`, the rest of `parse.c` (declarators, structs, initializers, the expression grammar) and `type.c` (type construction, `usual_arith_conv`, casts).
- The real compiler was built and run on C versions of the five examples. See the comparison at the end.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_ast` | Parsing and typing | `parse.c` (`function`, `declaration`, `funcall`, `compound_stmt`), `type.c` (`add_type`) | The `Node` and `Obj` records and the helpers that build them. Locals and globals are prepended to their lists. A function has the hidden local `__alloca_size__` and two string literals for `__func__` and `__FUNCTION__`, which use up the names `.L..0`, `.L..1`. `int x = e;` is a comma of `ND_MEMZERO` and an assignment. `while` is a `ND_FOR`. Every statement of a block is typed when the block is built, and a call is typed as it is built | The tokenizer, the preprocessor and the grammar: the examples are hardcoded as the calls the parser would make. One type, `int`, plus the type of a string literal and of a function. No implicit casts (`new_cast` would add `int` to `int` casts that generate no code), no `usual_arith_conv`, no pointer arithmetic scaling, no scopes, no `for` with `init` or `inc`, no `do`, `switch`, `goto`, labels, `?:`, `&&`, `||`, structs, floats |
| `stage_assign_offsets` | `assign_lvar_offsets` | `codegen.c` | Locals are visited from the newest to the oldest, each gets the next slot below `rbp` aligned to its own alignment, and the frame is rounded up to 16 | Parameters passed on the stack (offset above `rbp`), the 16-byte alignment of big arrays, structs |
| `stage_codegen` | `gen_expr` and `gen_stmt` | `codegen.c` | One walk over the tree. Every expression leaves its value in `rax`. A binary operation evaluates the right side, pushes it, evaluates the left side and pops into `rdi`. An assignment pushes the address first. A call pushes the arguments from last to first, evaluates the callee into `rax`, pops the arguments into `rdi`, `rsi`, ..., moves the callee to `r10`, sets `rax` to the number of vector registers and calls. `if`, `for` and `return` use the same labels and the same jump layout, `count()` numbers them, and `main` returns 0 when it falls off its end. The prologue stores the parameters from their registers, and `__alloca_size__` is set from `rsp`. The text matches what `chibicc -S` writes | `.loc`, `.file` and `.type`/`.size` directives, `-fPIC` and `@GOTPCREL` addressing, thread-local variables, floats, structs, bitfields, variable-length arrays, `alloca`, varargs. A function address always uses `lea name(%rip)`, where chibicc uses a GOT load for a function defined in another file. Strings are printed with `.string`, where chibicc writes one `.byte` per character. The one branch on the stack depth is a check that fails: a call at an odd depth would need the `sub $8, %rsp` padding, which no example has |
| `stage_encode` | The assembler and the linker | `main.c` runs `as` and `ld` (via `cc`) on the text | Not chibicc code: chibicc writes text and never encodes it. This stage encodes the instructions it produced into bytes, records labels and patches the rel32 fields of jumps and rip-relative addresses | Not read, since chibicc has no counterpart. It uses 8-bit displacements only, near jumps only, no ELF object, and the runtime (`print_int`, `print_str`) and the string data are placed after the code, where a real program takes them from libc and `.data` |
| `stage_export` | The loader | the OS | The code is placed in executable memory and `main` is called | An anonymous `mmap`, `mprotect` and a function pointer. `code.bin` holds the raw bytes |

## Deliberate deviations

- The runtime functions `print_int` and `print_str` are declared in the AST like prototypes, and their bytes are part of this file. `print_str` takes a NUL-terminated string like the real C string, and finds the length itself.
- Only 32-bit integers exist. Every instruction on a value is the `eax` form, and a load is `movsxd`. The other sizes of `load`, `store` and `store_gp` are dropped.
- The assembly is kept as a list of instructions instead of being printed as it is generated, because the encoder needs it. It is printed after the walk, with the same text.
- `add_type` does not visit the arguments of a call, because a call is typed when it is built (as in `funcall`) and the walk stops at a node that already has a type. The real code has the loop, which is dead for these inputs.
- The numbering of `.L..N` and `.L.else.N` follows the real compiler: the unique names come from the parser (and are used by `__func__` and by the break and continue labels of a loop), and `count()` is called when a statement is generated.

## Comparison with the real tool

Command: `chibicc -S -o x.s x.c`, built from the commit above, on C versions of each example with `int main(void)` and prototypes for `print_int` and `print_str`. The part from the first `.globl` was compared after removing `.loc`, `.type`, `.text` and the data directives, collapsing whitespace, and changing `mov f@GOTPCREL(%rip), %rax` to `lea f(%rip), %rax`.

- Matches: all five examples give the same text, instruction for instruction. That includes the order of the functions (`main` before `fact`, since chibicc walks its list from the newest object), the frame offsets (`__alloca_size__` at -8 and the first parameter at -12), the `rep stosb` that clears each initialized local, the label numbers and the layout of `if` and of the loop.
- Differs: `je` after `if` has two spaces in the real output, which does not matter to `as`. The directives and data listed above. Bytes were not compared: chibicc's output goes through `as` and this entry encodes its own text.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/chibicc/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.
