# tcc

A cut-down copy of the Tiny C Compiler for x86-64: one pass from tokens to machine code, with a value stack in between instead of a syntax tree or an IR.

## The idea

TCC parses and generates code at the same time. The parser calls `vpushi`, `gen_op`, `vstore` and friends as it recognizes the source, and those work on the value stack, `vstack`. This entry keeps that and skips the tokenizer: the C tokens of each example are written down by hand.

- **A value says where it is, not what to do.** An entry of the value stack is a constant, a local variable in memory, a register, a symbol, or the CPU flags. `n * fact(n - 1)` pushes `n` as "the local at rbp-8" and nothing is loaded. When `*` needs both operands in registers, `gv` loads the one that is not in a register yet.
- **Constants are never loaded for an add.** `cmp eax, 2` and `sub eax, 1` take the constant as an immediate. Only the left operand goes through a register.
- **Comparisons live in the flags.** `a < b` emits `cmp` and the stack entry becomes "flags: <". It is turned into a jump by the `if` or `while` that asks for it (`gvtst`), which emits the inverse condition. No register is ever set to 0 or 1.
- **Forward jumps are chains.** A jump to a place that does not exist yet is emitted with its rel32 field holding the previous such jump, and the offset of the field becomes the head of the chain. `gsym_addr` walks the chain and fills the real offsets in when the target is known. Strings placed after the code are referenced the same way.
- **The prologue is written last.** Space for `push rbp; mov rbp, rsp; sub rsp, N` is left blank, and the epilogue fills it in once the size of the frame is known.

Every entry is checked against the five programs in `examples/`, written as C here. The tokens, every value stack action with the instructions it emits, and the final bytes are in `dumps/<example>/stages.txt`. The machine code is in `dumps/<example>/code.bin`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source and the list of what was dropped. The short version: no tokenizer, no types beyond `int`, no spilling of registers, no `&&`, `||`, or flag-to-register conversion, and a runtime that is hand-assembled instead of libc.
