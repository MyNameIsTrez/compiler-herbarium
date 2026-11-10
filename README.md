# compiler-herbarium

A collection of heavily cut-down compilers and interpreters, each written from scratch to mimic the design of a well-known real one. Think of it as Rosetta Code for compiler architecture: one fixed task, many implementations, laid out so you can compare them side by side.

The repo is meant as a learning resource for people, and as a reference that LLMs can read when researching new ways to do compilation.

It holds two kinds of entries, kept in separate directories:

- `distilled/` holds toy versions of existing, open-source compilers and interpreters.
- `research/` holds new compilers produced by this repo that are not purely distilled versions of an existing design, such as a hybrid of two designs or a new idea tried on the same tasks.

## Why this repo exists

Real compilers are some of the largest and most carefully engineered programs there are. Clang and GCC are millions of lines. LuaJIT is small by comparison, but dense. The ideas that make each design what it is are buried in that code, which makes them hard to learn and almost impossible to combine. This repo pulls each design down to the part that makes it distinctive, in a single file that takes a second to build, and runs every design on the same programs.

Every entry keeps only what is interesting about its design, and leaves out the rest. That is why there is no tokenizer or parser: the examples are hardcoded in the form the design starts from. It is also why the entries leave out the optimizations that nearly every compiler has, such as generic constant folding, dead code elimination and common subexpression elimination, unless the design is built around them (Sea of Nodes is built around global value numbering and peepholes, so its entry has them). The goal is for each toy compiler to boil down to the essence that makes it worth studying when designing a new compiler.

That is useful in a few ways:

- **Learning.** You can read how a Sea of Nodes graph is built, how GCC lowers from GIMPLE to RTL, or how LuaJIT records a trace, in a few hundred lines. The committed dumps show the program after every stage, so you can follow the transformation without running anything.
- **Comparison.** Every entry runs the same examples, so the designs can be diffed side by side: what the IR looks like, which stages exist, how many instructions come out.
- **Grounding for LLMs.** A model asked about a compiler design usually answers from memory, and memory is unreliable for details. Each distilled entry is runnable code that is checked against the real source, so a model can read the real thing instead of guessing.
- **A testbed for new compiler designs.** This is the main reason the repo is shaped the way it is. Once a handful of designs exist in the same small, uniform form, a person or an LLM can invent a new paradigm by combining pieces of them, such as the graph IR of one with the trace recording of another, or by implementing an idea from a recent research paper. The result lives in `research/`, starts from a working baseline, and must pass the same examples, the same sanitizer builds and the same 100% coverage rule as everything else. The cost of an experiment is a few hundred lines of C and a few seconds, and a broken one fails loudly instead of quietly producing wrong output.

The repo is a set of specimens and a workbench, not a benchmark and not a production toolchain.

[`RESEARCH.md`](RESEARCH.md) is the running list of links behind it: the leads that already have an entry, and the ones that still need investigating.

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
gcc research/<compiler>/main.c
```

This repo is Linux only. Entries may rely on Linux system calls and Linux-specific memory mapping, and no effort is made to support other operating systems.

## Real compilers

These are the planned entries in `distilled/`, each a toy version of a real compiler or interpreter.

The JIT column says whether the real design compiles code while the program is running (✅), as opposed to compiling ahead of time or interpreting (❌). It is 🟡 when the answer depends on how the design is used, for example an IR that real systems use in both JIT and ahead-of-time compilers.

| Entry | Mimics | JIT | What it should show |
|-------|--------|:---:|---------------------|
| [`distilled/sea-of-nodes`](distilled/sea-of-nodes/) | Cliff Click's Sea of Nodes IR (see [SeaOfNodes/Simple](https://github.com/SeaOfNodes/Simple)) | 🟡 | Graph IR with floating nodes, scheduling, instruction selection |
| [`distilled/clang`](distilled/clang/) | Clang and LLVM | ❌ | AST-free lowering to SSA, mem2reg style cleanup, instruction selection, register allocation |
| `distilled/gcc` | GCC | ❌ | GIMPLE, RTL, and the pass pipeline |
| [`distilled/puc-lua`](distilled/puc-lua/) | The reference Lua interpreter | ❌ | Register bytecode and the dispatch loop |
| `distilled/cpython` | CPython | 🟡 | The compile pipeline from AST to a CFG of basic blocks to bytecode, and the eval loop. The JIT is optional and experimental |
| `distilled/chibicc` | chibicc, a small C compiler | ❌ | An AST walked by a code generator that emits assembly per node with a stack machine, the opposite end of the design space from the IR-based entries |
| `distilled/luajit` | LuaJIT | ✅ | Interpreter, hot trace recording, trace IR, machine code |

More designs can be added over time. The list is a plan, not a promise.

Only open-source compilers and interpreters get an entry. The real source code is required to distill a design down to its essence, and it is what each entry's `FIDELITY.md` and review pass are checked against. A design that is only documented, or whose source is closed (like MSVC), is out of scope.

## Research compilers

These are the entries in `research/`. They are new designs that do not have a single real counterpart, so they have no `FIDELITY.md`. Each one has a `README.md` that says what idea it tests and which distilled entries it builds on or differs from. The JIT column uses the same ✅, ❌ and 🟡 as above.

| Entry | Idea | JIT | Builds on |
|-------|------|:---:|-----------|
| `research/naive` | The simplest possible compiler: a stack machine IR where every instruction expands to a fixed run of x86-64 bytes, with no analysis and no optimization. It is the baseline the other entries are measured against. | ❌ | Nothing. Related to V8's Sparkplug, see its README |

## Layout of an entry

Each entry follows the same shape, so the same file names can be opened in different directories and compared. Distilled entries live in `distilled/<entry>/` and research entries in `research/<entry>/`:

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

Every entry is run as `./a.out <example> [code.bin]`, where `<example>` is the name of a directory in `examples/`:

- stdout is the program's output, which must equal `examples/<example>/expected.txt`.
- stderr is the intermediate form after each stage.
- If a path is given, the final machine code is written there.
- With no example, or an unknown one, the entry prints a usage message and exits with status 2.

The dumps in each entry's own `dumps/` directory are committed, so the intermediate forms can be read without building anything. They must be deterministic, which means no pointers, timestamps or other run-dependent values in them. `scripts/check-entry.sh` does the work:

```
scripts/check-entry.sh distilled/<compiler>            # build, run, check output, check dumps are up to date
scripts/check-entry.sh --update distilled/<compiler>   # also regenerate the committed dumps
```

It builds with:

```
gcc -std=c17 -O1 -g -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes -fsanitize=address,undefined
```

CI runs the script for every directory under `distilled/` and `research/`, at `-O1` and `-O2`, so a new entry is picked up without editing the workflow.

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
- ISO C forbids casting the `mmap` result straight to a function pointer, and `-Wpedantic` rejects it, so entries `memcpy` the pointer into a function-pointer variable instead. They also define `_DEFAULT_SOURCE` at the top, because strict `-std=c17` hides `mmap`.
- Each entry that generates machine code writes its final code bytes to the path it is given, which ends up as `dumps/<example>/code.bin`. It can be disassembled with `objdump -D -b binary -m i386:x86-64`. This replaces the object file you would get from a conventional toolchain.

`puc-lua` is an interpreter and generates no machine code, so it runs its bytecode in its own dispatch loop instead and has no `code.bin`.

## When is an entry done

A distilled entry is a toy version of a real design, so "done" has to mean more than "it prints the right text". It is done when all of these hold:

- `scripts/check-entry.sh` passes, which means the output is correct and the committed dumps are up to date.
- `scripts/coverage-entry.sh` passes, which means 100% line and branch coverage.
- `scripts/check-fidelity.sh` passes, which means every `stage_*` function in `main.c` is in the `FIDELITY.md` mapping table and the other way around.
- Every stage that defines the design is present, in the same order as in the real compiler. For example, GCC lowers from GIMPLE to RTL, and LuaJIT records a trace, builds trace IR and then emits machine code.
- Every stage is simplified, never dropped. A toy keeps the full pipeline and shrinks each stage, for example a register allocator that colors an interference graph but does no live-range splitting or coalescing, instead of leaving out register allocation or replacing it with a stack slot per value.
- Its `FIDELITY.md` is filled in. Each stage is mapped to the real compiler's stage name and a location in the real source, and every deliberate deviation is listed with a reason.
- A review pass has been done against the real source and, where the real tool can print its own intermediate forms, against those dumps. Useful tools are `clang -emit-llvm` with `-mllvm -print-after-all`, `gcc -fdump-tree-all` and `-fdump-rtl-all`, `luac -l -l`, and LuaJIT's `-jbc` and `-jdump`. Output will not be identical, but the stages, their order and the shape of the data should match.

A reviewer, human or LLM, is given those references and checks specific claims. Asking a model from memory whether a design matches is not enough, because it can misremember how the real compiler works. An LLM is useful for the mechanical checks, such as filling in the stage mapping, spotting missing stages and confirming that the listed deviations match the code. Whether the remaining deviations are acceptable is a human decision.

A research entry has no real counterpart to check against, so it is done when `scripts/check-entry.sh` and `scripts/coverage-entry.sh` pass and its `README.md` states the idea, the distilled entries it builds on and how it differs from them.

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

## Status

Just started. Four entries exist so far, `research/naive`, `distilled/puc-lua`, `distilled/sea-of-nodes` and `distilled/clang`, and all pass every example. The other distilled entries are still only planned.

## Non-goals

- Measuring or optimizing performance. The compilers stay tiny, so speed numbers would say little about the real designs.
- Competing with the real compilers on coverage.
- Parsing a real language, or including the optimizations that every compiler shares.
- Being a production tool. These are specimens, not products.
- GPU compilers. They mostly add a different target and runtime rather than a new compiler design, and they would need their own examples and a CI that can run GPU code.
