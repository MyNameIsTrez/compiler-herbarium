# Research dump

A running list of links to compiler designs, papers, posts and repos that are relevant to this repo. It has two tables:

1. Leads that already have a compiler in this repo.
2. Leads that do not have one yet, and still need to be investigated.

When a lead gets an entry, move its row from the second table to the first. New leads usually arrive as issues, see [CONTRIBUTING.md](CONTRIBUTING.md).

The "Read" column says whether the lead has been read in enough depth to rely on, not just listed. ✅ means read, ❌ means not read yet.

## Leads with a compiler

| Entry | Lead | Read | How it was used |
|-------|------|:----:|-----------------|
| [`research/naive`](research/naive/) | [Sparkplug blog post](https://v8.dev/blog/sparkplug) and V8's [`baseline-compiler.cc`](https://github.com/v8/v8/blob/main/src/baseline/baseline-compiler.cc) | ✅ | Compared against, not distilled. `naive` shares Sparkplug's central trick of expanding each instruction with no IR in between, and its README lists what matches and what differs. |
| [`distilled/puc-lua`](distilled/puc-lua/) | [lua/lua](https://github.com/lua/lua) at `v5.4.7`: `lopcodes.h`, `lvm.c`, `ldo.c`, `lfunc.c` | ✅ | Distilled. The bytecode is copied from the real `luac -l -l`, and a dispatch loop with only the executed opcodes follows `luaV_execute`. The compiler (`lparser.c`, `lcode.c`) was not read in depth and is not mimicked. |
| [`distilled/sea-of-nodes`](distilled/sea-of-nodes/) | [SeaOfNodes/Simple](https://github.com/SeaOfNodes/Simple) at `394f0aba`, chapter 21 | ✅ | Distilled. Read in depth: `CodeGen`, `Parser`, `GlobalCodeMotion`, `RegAlloc`, `BuildLRG`, `IFG`. Skimmed or not read: instruction selection, list scheduling, encoding, coalescing, which are modeled on the phase names only. The graph, global code motion and the graph-coloring allocator with splitting follow the source; the README ends with the lessons of the [V8 post](https://v8.dev/blog/leaving-the-sea-of-nodes). |

## Leads without a compiler yet

| Lead | Link | Read | Why it matters | What it could become |
|------|------|:----:|----------------|----------------------|
| Leaving the Sea of Nodes | [v8.dev/blog/leaving-the-sea-of-nodes](https://v8.dev/blog/leaving-the-sea-of-nodes) | ✅ | V8 moved Turbofan off Sea of Nodes to a CFG IR (Turboshaft), citing complexity, too few floating nodes and slow compiles. Compile time halved. | A section in the sea-of-nodes entry's README, and research ideas such as a hybrid that keeps effectful nodes in a CFG. |
| Turboshaft | [V8 source, `src/compiler/turboshaft`](https://github.com/v8/v8/tree/main/src/compiler/turboshaft) | ❌ | V8's CFG-based replacement for Sea of Nodes. The post above says a follow-up post will describe its design. | `distilled/turboshaft` |
| Clang and LLVM | [llvm/llvm-project](https://github.com/llvm/llvm-project) | ❌ | Planned. SSA IR, mem2reg, instruction selection, register allocation. | `distilled/clang` |
| GCC | [gcc.gnu.org](https://gcc.gnu.org/) | ❌ | Planned. GIMPLE, RTL and the pass pipeline. | `distilled/gcc` |
| CPython | [python/cpython](https://github.com/python/cpython) | ❌ | Suggested by the repo owner. A bytecode compiler with a different shape from Lua: AST, symbol table, CFG of basic blocks, then bytecode, run by an eval loop with an adaptive specializing interpreter and an optional copy-and-patch JIT. | `distilled/cpython` |
| LuaJIT | [LuaJIT/LuaJIT](https://github.com/LuaJIT/LuaJIT) | ❌ | Planned. Trace recording, trace IR and machine code. | `distilled/luajit` |
| Cranelift | [bytecodealliance/wasmtime, `cranelift`](https://github.com/bytecodealliance/wasmtime/tree/main/cranelift) | ❌ | A production code generator that is smaller than LLVM. Worth checking how its mid-end differs from the other designs here. | `distilled/cranelift`, or ideas for a research entry |
| QBE | [QBE mirror](https://github.com/omenos/mirror-qbe) (upstream: c9x.me/git/qbe.git) | ❌ | A small SSA backend, so a candidate for a minimal Clang-like back end. | Reference for `distilled/clang` |
| chibicc | [rui314/chibicc](https://github.com/rui314/chibicc) | ❌ | A small, readable C compiler built commit by commit. Suggested by the repo owner as a toy to write: it is already small, so cutting what the examples do not need should be easy. Its pipeline is tokenizer, parser, typed AST, and a code generator that expands each node into a fixed run of assembly with a stack machine. | `distilled/chibicc`, and a style reference |
| Crafting Interpreters | [craftinginterpreters.com](https://craftinginterpreters.com) | ❌ | Implements one language twice, as a tree-walker and a bytecode VM. A model for this repo's side-by-side format. | Layout reference |
| Are We Fast Yet | [Cross-Language Compiler Benchmarking](https://stefan-marr.de/papers/dls-marr-et-al-cross-language-compiler-benchmarking-are-we-fast-yet/) | ❌ | A set of 14 small benchmarks implemented across many languages. A source of ideas for later `examples/`, though this repo checks output rather than speed. | New examples |
| PL Zoo | [plzoo.andrej.com](https://plzoo.andrej.com/) | ❌ | Small implementations of several languages in one repo. The closest format to a zoo, but it varies the language, not the compiler design. | Layout reference |
| A Bestiary of Single-File Implementations of Programming Languages | [marcpaq/b1fipl](https://github.com/marcpaq/b1fipl) | ❌ | A catalogue of single-file language implementations. Source of candidate designs. | Candidate designs, layout reference |
| Rosetta Code compiler tasks | [Compiler/Sample programs](https://rosettacode.org/wiki/Compiler/Sample_programs) | ❌ | The same tiny-language pipeline implemented in many programming languages, with shared sample programs. | Ideas for later `examples/` |
| Nanopass and incremental compiler construction | [Ghuloum, An Incremental Approach to Compiler Construction](https://www.schemeworkshop.org/2006/11-ghuloum.pdf) | ❌ | One pipeline of many small passes with an IR per pass. | Possible `research/` entry on many tiny passes |
