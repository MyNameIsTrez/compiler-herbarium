# cpython

A cut-down version of CPython's bytecode compiler and eval loop: a tree turned into an instruction sequence, a control flow graph of basic blocks that is optimized, bytes with inline caches, and a stack machine that runs them.

## The idea

CPython is a compiler whose output is meant to be read by a fast interpreter, and every stage is shaped by that.

- **Scopes decide the opcodes.** The symbol table marks each name local or implicit global. A function uses `LOAD_FAST` and `LOAD_GLOBAL`, and a module, which has no fixed set of locals, uses `LOAD_NAME` and `STORE_NAME` on a dictionary.
- **Codegen is local and dumb.** It emits `LOAD_CONST`, a pseudo `JUMP`, a `TO_BOOL` before every condition and a `PUSH_NULL` before every call, and ends every body with `LOAD_CONST None; RETURN_VALUE`. Labels are only markers in a flat list.
- **The CFG is where the code gets better.** The blocks are rewritten in place. A small exit block is copied over a jump to it. Unreachable blocks go. A small integer gets its own instruction, `COMPARE_OP; TO_BOOL` and `LOAD_GLOBAL; PUSH_NULL` each become one instruction, and constant operands are folded.
- **The assembler works in code units.** Every instruction is one unit plus its inline cache entries, and a jump counts from the end of the instruction and its caches. A forward conditional jump gets a `NOT_TAKEN`, and a jump back is `JUMP_BACKWARD`.
- **The bytes are the real bytes.** The opcode numbers are CPython's, so each `code:` line in the dumps is the `co_code` of the real interpreter for the same program.
- **A stack machine runs them.** A call leaves the callable, a NULL slot and the arguments on the stack, and recurses.

Every entry is checked against the five programs in `examples/`, written here as Python. The AST, the scopes, every stage of the compiler and the trace of the run are in `dumps/<example>/stages.txt`. There is no `code.bin`, because the bytecode is for an interpreter, not for the CPU.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_ast` | The tree that the parser would build |
| `stage_symtable` | The scope of every name |
| `stage_codegen` | An instruction sequence with labels, for each code unit |
| `stage_cfg` | Splits the sequence into basic blocks |
| `stage_optimize_cfg` | Inlines exit blocks, removes dead blocks, rewrites instructions, drops unused constants |
| `stage_assemble` | Stack depth, `NOT_TAKEN`, borrowed loads, jump offsets and the bytes |
| `stage_run` | The eval loop, with a trace of each instruction |

## What it leaves out

No tokenizer or parser. No specialization: the cache entries are in the bytes and skipped by the loop, but nothing writes to them, and the adaptive interpreter and the JIT are absent. No line table, exception table or code object. Only the passes and the opcodes that the examples need.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to CPython's source, which files were read and which were not, and the comparison with the real interpreter, which agrees byte for byte on the examples.
