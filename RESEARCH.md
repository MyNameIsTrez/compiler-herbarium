# Research dump

A running list of links to compiler designs, papers, posts and repos that are relevant to this repo. It has three tables:

1. Leads that already have an entry in this repo.
2. Leads that do not have an entry yet, and still need to be investigated.
3. Leads that were considered and declined.

When a lead gets an entry, move its row from the second table to the first. New leads usually arrive as issues, see [CONTRIBUTING.md](CONTRIBUTING.md).

The "Read" column says whether the lead has been read in enough depth to rely on, not just listed. ✅ means read, ❌ means not read yet.

## Leads with an entry

| Entry | Lead | Read | How it was used |
|-------|------|:----:|-----------------|
| [`research/naive`](research/naive/) | [Sparkplug blog post](https://v8.dev/blog/sparkplug) and V8's [`baseline-compiler.cc`](https://github.com/v8/v8/blob/main/src/baseline/baseline-compiler.cc) | ✅ | Compared against, not distilled. `naive` shares Sparkplug's central trick of expanding each instruction with no IR in between, and its README lists what matches and what differs. |
| [`distilled/puc-lua`](distilled/puc-lua/) | [lua/lua](https://github.com/lua/lua) at `v5.4.7`: `lopcodes.h`, `lvm.c`, `ldo.c`, `lfunc.c` | ✅ | Distilled. The bytecode is copied from the real `luac -l -l`, and a dispatch loop with only the executed opcodes follows `luaV_execute`. The compiler (`lparser.c`, `lcode.c`) was not read in depth and is not mimicked. |
| [`distilled/sea-of-nodes`](distilled/sea-of-nodes/) | [SeaOfNodes/Simple](https://github.com/SeaOfNodes/Simple) at `394f0aba`, chapter 21 | ✅ | Distilled. Read in depth: `CodeGen`, `Parser`, `GlobalCodeMotion`, `RegAlloc`, `BuildLRG`, `IFG`. Skimmed or not read: instruction selection, list scheduling, encoding, coalescing, which are modeled on the phase names only. The graph, global code motion and the graph-coloring allocator with splitting follow the source; the README ends with the lessons of the [V8 post](https://v8.dev/blog/leaving-the-sea-of-nodes). |
| [`distilled/clang`](distilled/clang/) | [llvm/llvm-project](https://github.com/llvm/llvm-project) at `eb461b1b` | ✅ | Distilled. Read in depth: `PromoteMemoryToRegister.cpp`, `RegAllocBase.cpp`, `RegAllocBasic.cpp`, `PHIElimination.cpp`, the hint and regmask code, clang's `CGStmt.cpp` block names. Skimmed or not read: the SelectionDAG combines and builder, live interval construction, the coalescer, two-address, frame lowering, encoding, which are modeled on the pass names and on `llc` dumps. The IR and the mem2reg result were compared with the real clang 23.1.3. |
| [`distilled/chibicc`](distilled/chibicc/) | [rui314/chibicc](https://github.com/rui314/chibicc) at `90d1f7f1` | ✅ | Distilled. Read in depth: the parts of `codegen.c` the examples use (`gen_expr`, `gen_stmt`, `assign_lvar_offsets`, `emit_text`), `parse.c` `function`, `funcall` and the naming helpers, and `type.c` `add_type`. Not read: the tokenizer, preprocessor, driver and most of the parser. The real compiler was built and its assembly for all five examples matches the entry's text. `stage_encode` is not chibicc code, since chibicc leaves encoding to `as`. |
| [`distilled/cpython`](distilled/cpython/) | [python/cpython](https://github.com/python/cpython) at `9d05428b` | ✅ | Distilled. Read in depth: `compile.c`, `codegen.c` for the constructs the examples use, `flowgraph.c` (block building, `optimize_cfg` and its helpers, stack depth, jump normalization, `optimize_load_fast`), `assemble.c` (instruction emission, jump offsets). Not read: `symtable.c`, `ceval.c`, `bytecodes.c`, `specialize.c`, the line and exception tables, the JIT. The real interpreter was built, and the entry's `co_code`, stack size, names and constants equal the real ones for all five examples. |

## Leads without an entry yet

| Lead | Link | Read | Why it matters | What it could become |
|------|------|:----:|----------------|----------------------|
| Leaving the Sea of Nodes | [v8.dev/blog/leaving-the-sea-of-nodes](https://v8.dev/blog/leaving-the-sea-of-nodes) | ✅ | V8 moved Turbofan off Sea of Nodes to a CFG IR (Turboshaft), citing complexity, too few floating nodes and slow compiles. Compile time halved. | A section in the sea-of-nodes entry's README, and research ideas such as a hybrid that keeps effectful nodes in a CFG. |
| Turboshaft | [V8 source, `src/compiler/turboshaft`](https://github.com/v8/v8/tree/main/src/compiler/turboshaft) | ❌ | V8's CFG-based replacement for Sea of Nodes. The post above says a follow-up post will describe its design. | `distilled/turboshaft` |
| GCC | [gcc.gnu.org](https://gcc.gnu.org/) | ❌ | Planned. GIMPLE, RTL and the pass pipeline. | `distilled/gcc` |
| LuaJIT | [LuaJIT/LuaJIT](https://github.com/LuaJIT/LuaJIT) | ❌ | Planned. Trace recording, trace IR and machine code. | `distilled/luajit` |
| GHC | [gitlab.haskell.org/ghc/ghc](https://gitlab.haskell.org/ghc/ghc) | ❌ | The most used Haskell compiler. Lazy functional design: Core, the simplifier, STG, then Cmm and code generation. Very different from the imperative IRs here. Suggested by the repo owner. | `distilled/ghc` |
| Lisp compilers | [SBCL](https://github.com/sbcl/sbcl) and [Chez Scheme](https://github.com/cisco/ChezScheme) | ❌ | The most used Lisp compiler and a famous Scheme compiler built from many small passes (nanopass). Candidates for a Lisp entry. Suggested by the repo owner. | `distilled/sbcl` or `distilled/chez-scheme` |
| rustc | [rust-lang/rust](https://github.com/rust-lang/rust) | ❌ | Rust's pipeline: HIR, THIR, MIR with borrow checking and monomorphization, then LLVM. A MIR-centered entry would show a mid-level IR that carries ownership. Suggested by the repo owner. | `distilled/rustc` |
| Zig | [ziglang/zig](https://github.com/ziglang/zig) | ❌ | ZIR and AIR, with comptime evaluation, and its own self-hosted back ends beside LLVM. Suggested by the repo owner. | `distilled/zig` |
| Cranelift | [bytecodealliance/wasmtime, `cranelift`](https://github.com/bytecodealliance/wasmtime/tree/main/cranelift) | ❌ | A production code generator that is smaller than LLVM. Worth checking how its mid-end differs from the other designs here. | `distilled/cranelift`, or ideas for a research entry |
| QBE | [QBE mirror](https://github.com/omenos/mirror-qbe) (upstream: c9x.me/git/qbe.git) | ❌ | A small SSA backend, so a candidate for a minimal Clang-like back end. | Reference for `distilled/clang` |
| Crafting Interpreters | [craftinginterpreters.com](https://craftinginterpreters.com) | ❌ | Implements one language twice, as a tree-walker and a bytecode VM. A model for this repo's side-by-side format. | Layout reference |
| Are We Fast Yet | [Cross-Language Compiler Benchmarking](https://stefan-marr.de/papers/dls-marr-et-al-cross-language-compiler-benchmarking-are-we-fast-yet/) | ❌ | A set of 14 small benchmarks implemented across many languages. A source of ideas for later `examples/`, though this repo checks output rather than speed. | New examples |
| PL Zoo | [plzoo.andrej.com](https://plzoo.andrej.com/) | ❌ | Small implementations of several languages in one repo. The closest format to a zoo, but it varies the language, not the compiler design. | Layout reference |
| A Bestiary of Single-File Implementations of Programming Languages | [marcpaq/b1fipl](https://github.com/marcpaq/b1fipl) | ❌ | A catalogue of single-file language implementations. Source of candidate designs. | Candidate designs, layout reference |
| Rosetta Code compiler tasks | [Compiler/Sample programs](https://rosettacode.org/wiki/Compiler/Sample_programs) | ❌ | The same tiny-language pipeline implemented in many programming languages, with shared sample programs. | Ideas for later `examples/` |
| Nanopass and incremental compiler construction | [Ghuloum, An Incremental Approach to Compiler Construction](https://www.schemeworkshop.org/2006/11-ghuloum.pdf) | ❌ | One pipeline of many small passes with an IR per pass. | Possible `research/` entry on many tiny passes |

## Considered and declined

These do not pass "What earns an entry" in the [README](README.md).

| Lead | Why declined |
|------|--------------|
| Brainfuck compilers | Eight commands map nearly one-to-one onto machine code. There is no IR, scoping or type system, so the compiler is a table. `research/naive` already shows expanding code with no IR in between. |
| A language that only lowers to LLVM IR | The back end would repeat `distilled/clang`. The front end, a language-specific lowering, is the part this repo leaves out. A design whose middle IR is the interesting part, such as MIR in rustc, is judged on that IR instead. |
