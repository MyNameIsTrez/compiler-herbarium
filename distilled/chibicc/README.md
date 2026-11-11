# chibicc

A cut-down version of chibicc, a small C compiler: a typed AST, one pass that gives each local a stack slot, and a code generator that expands every node into a fixed run of x86-64 instructions.

## The idea

Most entries in this repo have an IR and a register allocator. chibicc has neither, and shows how little a working compiler needs.

- **The tree is the only IR.** The parser builds `Node`s and `type.c` gives each one a type. Nothing is rewritten after that.
- **The machine stack holds every temporary.** Every expression leaves its value in `rax`. A binary operation evaluates its right side and pushes it, evaluates its left side, and pops the right side into `rdi`. There are two registers in play and no allocator. `depth` counts the pushes that are still open, so a call knows whether the stack is 16-byte aligned.
- **Locals live in memory.** `assign_lvar_offsets` gives each local a slot below `rbp`, newest first. A parameter is copied from its register to its slot in the prologue. Every read of a variable is `lea` then `movsxd`.
- **A node is a template.** `if` is a compare, `je` to the else label, the branch, `jmp` to the end label. A call pushes the arguments from the last to the first and pops them into the argument registers. There is no choice to make and nothing to optimize.
- **An initialized local is two nodes.** `int x = 3;` becomes `rep stosb` over the whole variable, then an assignment.
- **Text out, not bytes.** chibicc writes assembly and lets `as` and `ld` finish. This entry's `stage_encode` stands in for them, so that the result can run in memory.

The entry is checked against the five programs in [`examples/`](examples/). The AST, the frame layout, the assembly and the encoded bytes are in `dumps/<example>/stages.txt`, and the machine code is in `dumps/<example>/code.bin`.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_ast` | Builds and types the AST the way the parser would |
| `stage_assign_offsets` | Gives every local a slot and sizes the frame |
| `stage_codegen` | Walks the tree and writes the assembly of each node |
| `stage_encode` | Encodes the instructions and patches the `rel32` fields |
| `stage_export` | Maps the bytes executable and runs `main` |

## What it leaves out

No tokenizer, preprocessor or grammar. Only `int`: no pointers, structs, floats, casts or arrays other than string literals. Only `if` and `while`. The generated text is the same as the real compiler's for the examples, and the comparison is in the fidelity file.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to chibicc's source, which files were read and which were not, and the comparison with the real compiler's output.
