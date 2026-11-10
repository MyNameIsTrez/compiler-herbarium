# compiler-herbarium

A collection of heavily cut-down compilers and interpreters, each written from scratch to mimic the design of a well-known real one. Think of it as Rosetta Code for compiler architecture: one fixed task, many implementations, laid out so you can compare them side by side.

The repo is meant as a learning resource for people, and as a reference that LLMs can read when researching new ways to do compilation.

![The design space of the entries: what sits between the source and execution, against when machine code is made. Filled dots are finished entries, dashed dots are planned ones.](docs/design-space.svg)

The map shows where each design sits, and where nothing sits yet. Finished entries are filled dots. Planned entries, which are leads in [RESEARCH.md](RESEARCH.md) that look worth an entry, are dashed dots, and they are not in the tables below until they pass every example. The placement is a judgement call, and `scripts/check-map.sh` checks only that every entry is on it. A crossed cell is one this repo has ruled out for good.

**What the map does not show.** It places a design by its pipeline, so some properties are invisible on it:

- *Compilers derived from an interpreter.* `luajit-remake`, PyPy and Truffle get their compiler from an interpreter (stencils, meta-tracing, partial evaluation), and sit next to designs that were written by hand.
- *Memory management.* The reference counting of Lean 4 is part of its IR, but no axis shows it.
- *Specialization and compile-time execution.* Julia's type specialization and Zig's comptime are not a point on the when-is-machine-code-made axis.
- *Several IRs at once.* `gcc` and MLIR pass through more than one IR, and sit where their middle IR does.
- *Tools about compilers.* Verifiers and superoptimizers of rewrite rules analyze the compiler, not a program, so they have no cell (see the declined table in [RESEARCH.md](RESEARCH.md)).

**One goal of this repository is to fill out this map.** Think of it as a bingo board: each cell is a combination of what sits between the source and execution and when machine code is made, and the aim is for every cell that a real compiler occupies to hold a distilled entry. A distilled entry comes first, because it is checked against real source. Where a cell is empty and a search finds no open-source compiler or interpreter that sits there, the repo adds an `original/` entry instead, shown as an amber diamond, so the board is covered by designs that are at least tried out. A few cells will stay empty because the cell cannot hold a design or holds only front-end work. They are crossed out on the map, and [RESEARCH.md](RESEARCH.md) gives the one-line reason for each.

It holds two kinds of entries, kept in separate directories:

- `distilled/` holds toy versions of existing, open-source compilers and interpreters.
- `original/` holds new compilers produced by this repo that are not purely distilled versions of an existing design, such as a hybrid of two designs or a new idea tried on the same tasks.

## Why this repo exists

Real compilers are some of the largest and most carefully engineered programs there are. Clang and GCC are millions of lines. LuaJIT is small by comparison, but dense. The ideas that make each design what it is are buried in that code, which makes them hard to learn and almost impossible to combine. This repo pulls each design down to the part that makes it distinctive, in a single file that takes a second to build, and runs every design on the same programs.

Every entry keeps only what is interesting about its design, and leaves out the rest. That is why there is no tokenizer or parser: the examples are hardcoded in the form the design starts from. It is also why the entries leave out the optimizations that nearly every compiler has, such as generic constant folding, dead code elimination and common subexpression elimination, unless the design is built around them (Sea of Nodes is built around global value numbering and peepholes, so its entry has them). The goal is for each toy compiler to boil down to the essence that makes it worth studying when designing a new compiler.

That is useful in a few ways:

- **Learning.** You can read how a Sea of Nodes graph is built, how GCC lowers from GIMPLE to RTL, or how LuaJIT records a trace, in a few hundred lines. The committed dumps show the program after every stage, so you can follow the transformation without running anything.
- **Comparison.** Every entry runs the same examples, so the designs can be diffed side by side: what the IR looks like, which stages exist, how many instructions come out.
- **Grounding for LLMs.** A model asked about a compiler design usually answers from memory, and memory is unreliable for details. Each distilled entry is runnable code that is checked against the real source, so a model can read the real thing instead of guessing.
- **A map to the real thing.** An entry is a starting point, not a substitute for the compiler it mimics. Each `FIDELITY.md` names the real source at a pinned commit or tag, says which parts were read and which were not, and lists what the entry dropped or simplified. A person or an LLM weighing several designs can compare the entries first, then clone the real compiler at that commit and read the dropped part when one of the designs looks right.
- **A testbed for new compiler designs.** This is the main reason the repo is shaped the way it is. Once a handful of designs exist in the same small, uniform form, a person or an LLM can invent a new paradigm by combining pieces of them, such as the graph IR of one with the trace recording of another, or by implementing an idea from a recent research paper. The result lives in `original/`, starts from a working baseline, and must pass the same examples, the same sanitizer builds and the same 100% coverage rule as everything else. The cost of an experiment is a few hundred lines of C and a few seconds, and a broken one fails loudly instead of quietly producing wrong output.

The repo is a set of specimens and a workbench, not a benchmark and not a production toolchain.

[`RESEARCH.md`](RESEARCH.md) is the running list of links behind it: the leads that still need investigating, and the ones that were declined.

## The idea

Every entry compiles (or interprets) the same set of tiny programs, listed in [`examples/`](examples/). They go from hello world up to a recursive function. Each is defined by a short Lua program and its exact expected output, for example:

```lua
print("Hello, world!")
```

There is no tokenizer and no parser. Each entry starts from a hardcoded data structure in the form its real counterpart would use internally, such as a Sea of Nodes graph, an SSA control flow graph or a bytecode prototype. From there it runs the stages that are characteristic of the design and produces either machine code or a running interpreter loop.

What distinguishes the designs lives in the middle of the pipeline, so each entry **dumps its intermediate form after every stage**. The dumps are the main output. Reading them in order shows how a design thinks, and diffing them across entries shows where the designs differ.

## Language and platform

Every compiler and interpreter in this repo is written in C17, so differences between entries come from the design rather than from the implementation language.

Every entry is a single, self-contained `main.c`. There are no shared C files and no headers, so each entry can be read, copied or fed to an LLM on its own. Small helpers such as the `mmap` code runner are deliberately duplicated in each entry. The only dependencies are libc and Linux.

Every entry builds with a single command, and this works for all of them:

```
gcc distilled/<compiler>/main.c
gcc original/<compiler>/main.c
```

This repo is Linux only. Entries may rely on Linux system calls and Linux-specific memory mapping, and no effort is made to support other operating systems.

## What earns an entry

An entry is a compiler design, not a language. It needs at least one of these:

- a distinct IR or program representation (Sea of Nodes, a CFG of basic blocks, a typed AST walked directly),
- a distinct execution model (a trace JIT, copy-and-patch, a stack machine with no register allocator), or
- semantic machinery that the compiler itself carries out (compile-time evaluation, borrow checking, lazy evaluation).

A language that only differs in surface syntax does not qualify, such as brainfuck, whose compiler is a table. Nor does a compiler that is a thin layer over an existing back end: its back end would repeat `distilled/clang`, and its front end is the part this repo leaves out. A design whose middle IR is the interesting part does qualify, even if LLVM comes last. Declined leads are recorded in [RESEARCH.md](RESEARCH.md).

## Real compilers

These are the entries in `distilled/`, each a toy version of a real compiler or interpreter.

The Lines of C column is `cloc`'s count of code lines in the entry's `main.c`, without blanks and comments. `scripts/check-cloc.sh` checks it and `scripts/check-cloc.sh --update` rewrites it.

The Kind column says whether the real design is a compiler or an interpreter. A design that compiles to bytecode and then runs it, like CPython, is an interpreter.

The JIT column says whether the real design compiles code to machine code while the program is running (✅), as opposed to doing so ahead of time or never (❌). It is 🟡 when the answer depends on how the design is used, for example an IR that real systems use in both JIT and ahead-of-time compilers.

| Entry | Lines of C | Mimics | Kind | JIT | What it shows |
|-------|-----------:|--------|------|:---:|---------------------|
| [`distilled/chibicc`](distilled/chibicc/) | 987 | chibicc, a small C compiler | Compiler | ❌ | An AST walked by a code generator that emits assembly per node with a stack machine, the opposite end of the design space from the IR-based entries |
| [`distilled/clang`](distilled/clang/) | 1745 | Clang and LLVM | Compiler | ❌ | AST-free lowering to SSA, mem2reg style cleanup, instruction selection, register allocation |
| [`distilled/clox`](distilled/clox/) | 888 | The bytecode interpreter of Crafting Interpreters | Interpreter | ❌ | A single-pass Pratt compiler that emits stack bytecode with no AST, locals as stack slots, and closures whose upvalues point into the stack until they are closed |
| [`distilled/cpython`](distilled/cpython/) | 1158 | CPython | Interpreter | 🟡 | The compile pipeline from AST to a CFG of basic blocks to bytecode, and the eval loop. The entry has no JIT: real CPython has an optional, experimental one (hence 🟡), and none of its code was read or copied |
| [`distilled/gcc`](distilled/gcc/) | 1750 | GCC | Compiler | ❌ | GENERIC to GIMPLE to SSA to RTL, with PHI placement, out-of-SSA partitions, combine and a graph-coloring register allocator with copy preferences |
| [`distilled/jlox`](distilled/jlox/) | 703 | The tree-walking interpreter of Crafting Interpreters | Interpreter | ❌ | A resolver that binds each variable use to a scope distance before the program runs, and a tree walker over a chain of environments with closures |
| [`distilled/luajit`](distilled/luajit/) | 756 | LuaJIT | Interpreter | ✅ | A tracing JIT: a hot loop is recorded into SSA IR with guards and snapshots, unrolled by copy-substitution with PHIs, and assembled backwards with register allocation. Only the `loop` example makes a trace. |
| [`distilled/luajit-interpreter`](distilled/luajit-interpreter/) | 948 | LuaJIT | Interpreter | ✅ | The assembler interpreter as x86-64 machine code generated at startup: pinned registers, dispatch at the end of every handler, tables as the mode switch, hot counters, a recording hook, and an exit handler. The hot loop of `loop` gets a template trace. |
| [`distilled/luajit-remake`](distilled/luajit-remake/) | 407 | LuaJIT Remake and Deegen | Interpreter | ✅ | A copy-and-patch baseline JIT: stencils with holes, copied and patched per bytecode, with the jump to the next bytecode cut off. The entry has only the JIT, not the interpreter that the real VM tiers up from |
| [`distilled/puc-lua`](distilled/puc-lua/) | 491 | The reference Lua interpreter | Interpreter | ❌ | Register bytecode and the dispatch loop |
| [`distilled/qbe`](distilled/qbe/) | 1630 | QBE | Compiler | ❌ | Pruned SSA from an IL with mutable temporaries, the ABI as register copies, backward instruction selection, and a backward register allocator with hints and parallel moves on edges |
| [`distilled/sea-of-nodes`](distilled/sea-of-nodes/) | 1401 | Cliff Click's Sea of Nodes IR (see [SeaOfNodes/Simple](https://github.com/SeaOfNodes/Simple)) | Compiler | 🟡 | Graph IR with floating nodes, scheduling, instruction selection |
| [`distilled/tcc`](distilled/tcc/) | 724 | The Tiny C Compiler | Compiler | ❌ | One-pass code generation from a lazy value stack: constants and locals loaded only when needed, comparisons kept in the CPU flags, forward jumps as chains patched later |

Every row is a finished entry. Designs that are planned or still being investigated are leads in [RESEARCH.md](RESEARCH.md), and a lead gets a row here only when its entry passes every example.

Only open-source compilers and interpreters get an entry. The real source code is required to distill a design down to its essence, and it is what each entry's `FIDELITY.md` and review pass are checked against. A design that is only documented, or whose source is closed (like MSVC), is out of scope.

## Original compilers

These are the entries in `original/`. They are new designs that do not have a single real counterpart, so they have no `FIDELITY.md`. Each one has a `README.md` that says what idea it tests and which distilled entries it builds on or differs from. The Kind and JIT columns use the same values as above.

| Entry | Lines of C | Idea | Kind | JIT | Builds on |
|-------|-----------:|------|------|:---:|-----------|
| `original/naive` | 284 | The simplest possible compiler: a stack machine IR where every instruction expands to a fixed run of x86-64 bytes, with no analysis and no optimization. It is the baseline the other entries are measured against. | Compiler | ❌ | Nothing. Related to V8's Sparkplug, see its README |

## Layout of an entry

Each entry follows the same shape, so the same file names can be opened in different directories and compared. Distilled entries live in `distilled/<entry>/` and original entries in `original/<entry>/`:

```
<entry>/
  main.c            the whole compiler: the hardcoded input, then one clearly marked section per stage
  README.md         what the design is, what this version leaves out
  FIDELITY.md       how each stage maps to the real compiler (distilled entries only, see templates/FIDELITY.md)
  dumps/<example>/stages.txt  the intermediate form after each stage, for each example
  dumps/<example>/code.bin    the final machine code for each example, for entries that generate any
```

Entries keep the same section order and the same `stage_*` function naming, so two `main.c` files can be diffed against each other.

## Building, running and checking

Every entry builds with plain gcc and plain clang, with no flags, for example `gcc distilled/cpython/main.c`, and is then run as `./a.out <example> [code.bin]`, where `<example>` is the name of a directory in `examples/`:

- stdout is the program's output, which must equal `examples/<example>/expected.txt`.
- stderr is the intermediate form after each stage.
- If a path is given, the final machine code is written there.

The dumps in each entry's own `dumps/` directory are committed, so the intermediate forms can be read without building anything. They must be deterministic, which means no pointers, timestamps or other run-dependent values in them. `scripts/check-entry.sh` does the work:

```
scripts/check-entry.sh distilled/<compiler>            # build, run, check output, check dumps are up to date
scripts/check-entry.sh --update distilled/<compiler>   # also regenerate the committed dumps
```

The script builds at `-O2` by default, the same level CI uses, because it is the level most likely to expose undefined behavior and flow-based warnings, so a local pass means a CI pass (`OPT=-O1` selects another level). CI runs it for every directory under `distilled/` and `original/`, with both gcc and clang (`CC=clang scripts/check-entry.sh ...`), so a new entry is picked up without editing the workflow. With clang the script adds `-fno-sanitize=function`, because that check crashes on calls into generated machine code. CI also compiles every `main.c` once with plain `gcc main.c` and plain `clang main.c`, with no flags.

### Coverage

Every entry must have 100% line and branch coverage, so there is as little cruft in the C code as possible. An entry's only inputs are the examples, so running all of them has to reach every line and take every branch of the compiler. Code that no example needs does not belong in the entry, and neither does a branch whose other side never runs. The script also runs the entry with no arguments and with an unknown example, and once without a `code.bin` path, so the usage and optional-argument branches count too.

```
scripts/coverage-entry.sh distilled/<compiler>
```

The script needs `gcc` and [gcovr](https://gcovr.com) (`pipx install gcovr==8.8`). It is a separate build from the one above, at `-O0` and without sanitizers, because the optimizer and the sanitizers add compiler-generated branches that no run can take. CI runs it for every entry.

This also keeps entries honest about what they leave out. A stage that needs a path no example takes, such as register spilling, is either dropped and listed under "Dropped or simplified" in `FIDELITY.md`, or exercised by a new example that is added to every entry.

Some code can never run, such as the failure path of a failed `mmap`. It may be excluded with a gcovr comment (`// GCOVR_EXCL_LINE`, `// GCOVR_EXCL_BR_LINE`, or a `GCOVR_EXCL_START` to `GCOVR_EXCL_STOP` block), and each exclusion should say why on the same line. Use them sparingly, since every exclusion is cruft the check cannot see.

## Running generated code

Every entry that generates machine code runs it the same way: the code is copied into an `mmap`ed buffer, the buffer is made executable with `mprotect`, and the entry's own `main.c` calls it in-process. No entry writes an ELF file or invokes an assembler or linker, so the only thing that differs between entries is the compilation design.

- The generated code is a function that takes one integer argument and returns an integer. It returns to `main` instead of exiting.
- It prints with raw `write` syscalls and makes no libc calls. Printing an integer therefore includes converting it to decimal in the generated code.
- A string lives in the same buffer as the code and is reached with a RIP-relative `lea`, so there is no data section or relocation to model.
- The three rules above are the default for ahead-of-time compilers. A design whose point is a runtime that the generated code calls into may differ. `distilled/luajit-remake` passes a frame pointer, calls a C helper to print, and reads constants from memory at addresses patched into the code.
- ISO C forbids casting the `mmap` result straight to a function pointer, and `-Wpedantic` rejects it, so entries `memcpy` the pointer into a function-pointer variable instead. They also define `_DEFAULT_SOURCE` at the top, because strict `-std=c17` hides `mmap`.
- Each entry that generates machine code writes its final code bytes to the path it is given, which ends up as `dumps/<example>/code.bin`. It can be disassembled with `objdump -D -b binary -m i386:x86-64`. This replaces the object file you would get from a conventional toolchain.
- An interpreter generates no machine code, so it runs its bytecode in its own dispatch loop instead and has no `code.bin`.

## When is an entry done

A distilled entry is a toy version of a real design, so "done" has to mean more than "it prints the right text". It is done when all of these hold:

- `scripts/check-entry.sh` passes, which means the output is correct and the committed dumps are up to date.
- `scripts/coverage-entry.sh` passes, which means 100% line and branch coverage.
- `scripts/check-fidelity.sh` passes, which means every `stage_*` function in `main.c` is in the `FIDELITY.md` mapping table and the other way around.
- `scripts/check-cloc.sh` passes, so the Lines of C column of the README tables is current. Run it with `--update` after changing a `main.c`.
- Every stage that defines the design is present, in the same order as in the real compiler. For example, GCC lowers from GIMPLE to RTL, and LuaJIT records a trace, builds trace IR and then emits machine code.
- Every stage is simplified, never dropped. A toy keeps the full pipeline and shrinks each stage, for example a register allocator that colors an interference graph but does no live-range splitting or coalescing, instead of leaving out register allocation or replacing it with a stack slot per value.
- Its `FIDELITY.md` is filled in. Each stage is mapped to the real compiler's stage name and a location in the real source, and every deliberate deviation is listed with a reason.
- A review pass has been done against the real source and, where the real tool can print its own intermediate forms, against those dumps. Useful tools are `clang -emit-llvm` with `-mllvm -print-after-all`, `gcc -fdump-tree-all` and `-fdump-rtl-all`, `luac -l -l`, and LuaJIT's `-jbc` and `-jdump`. Output will not be identical, but the stages, their order and the shape of the data should match.

A reviewer, human or LLM, is given those references and checks specific claims. Asking a model from memory whether a design matches is not enough, because it can misremember how the real compiler works. An LLM is useful for the mechanical checks, such as filling in the stage mapping, spotting missing stages and confirming that the listed deviations match the code. Whether the remaining deviations are acceptable is a human decision.

An original entry has no real counterpart to check against, so it is done when `scripts/check-entry.sh` and `scripts/coverage-entry.sh` pass and its `README.md` states the idea, the distilled entries it builds on and how it differs from them.

## Examples

Hello world alone makes most designs look alike, because the differences show up with control flow and with values that live across instructions. The examples in [`examples/`](examples/) are a ladder, and every entry has to pass every rung:

| Example | What it adds |
|---------|--------------|
| `hello` | Printing a string |
| `arithmetic` | Integer `+`, `-` and `*` with constants |
| `branch` | Locals, a comparison and an `if` with an `else` |
| `loop` | A `while` loop that updates locals |
| `call` | A recursive function with an argument and a return value |

A new example is added to every entry, the same way a Rosetta Code task is. See [`examples/README.md`](examples/README.md) for the exact subset of Lua that the programs use.

Examples are checked for correct output, not timed. Designs can still be compared with numbers that do not need a stopwatch, such as emitted instruction count, code size in bytes, or the number of IR nodes at each stage.

## Non-goals

- Measuring or optimizing performance. The compilers stay tiny, so speed numbers would say little about the real designs.
- Parsing a real language, or including the optimizations that every compiler shares.
- Being a production tool. These are specimens, not products.
- Compilers for languages that add no compiler design, see "What earns an entry".
- GPU compilers. They mostly add a different target and runtime rather than a new compiler design, and they would need their own examples and a CI that can run GPU code.
