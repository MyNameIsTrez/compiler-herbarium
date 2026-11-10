# compiler-herbarium

A collection of heavily cut-down compilers and interpreters, each written from scratch to mimic the design of a well-known real one. Think of it as Rosetta Code for compiler architecture: one fixed task, many implementations, laid out so you can compare them side by side.

The repo is meant as a learning resource for people, and as a reference that LLMs can read when researching new ways to do compilation.

It holds two kinds of entries, kept in separate directories:

- `distilled/` holds toy versions of existing, open-source compilers and interpreters.
- `research/` holds new compilers produced by this repo that are not purely distilled versions of an existing design, such as a hybrid of two designs or a new idea tried on the same tasks.

## The idea

Every entry compiles (or interprets) the same tiny program, effectively:

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
| `distilled/sea-of-nodes` | Cliff Click's Sea of Nodes IR (see [SeaOfNodes/Simple](https://github.com/SeaOfNodes/Simple)) | 🟡 | Graph IR with floating nodes, scheduling, instruction selection |
| `distilled/clang` | Clang and LLVM | ❌ | AST-free lowering to SSA, mem2reg style cleanup, instruction selection, register allocation |
| `distilled/gcc` | GCC | ❌ | GIMPLE, RTL, and the pass pipeline |
| `distilled/puc-lua` | The reference Lua interpreter | ❌ | Register bytecode and the dispatch loop |
| `distilled/luajit` | LuaJIT | ✅ | Interpreter, hot trace recording, trace IR, machine code |

More designs can be added over time. The list is a plan, not a promise.

Only open-source compilers and interpreters get an entry. The real source code is required to distill a design down to its essence, and it is what each entry's `FIDELITY.md` and review pass are checked against. A design that is only documented, or whose source is closed (like MSVC), is out of scope.

## Research compilers

These are the entries in `research/`. They are new designs that do not have a single real counterpart, so they have no `FIDELITY.md`. Each one has a `README.md` that says what idea it tests and which distilled entries it builds on or differs from. The JIT column uses the same ✅, ❌ and 🟡 as above.

| Entry | Idea | JIT | Builds on |
|-------|------|:---:|-----------|
| None yet | | | |

## Layout of an entry

Each entry follows the same shape, so the same file names can be opened in different directories and compared. Distilled entries live in `distilled/<entry>/` and research entries in `research/<entry>/`:

```
<entry>/
  main.c            the whole compiler: the hardcoded input, then one clearly marked section per stage
  README.md         what the design is, what this version leaves out
  FIDELITY.md       how each stage maps to the real compiler (distilled entries only, see templates/FIDELITY.md)
  dumps/stages.txt  the intermediate form after each stage
  dumps/code.bin    the final machine code, for entries that generate any
```

Entries keep the same section order and the same `stage_*` function naming, so two `main.c` files can be diffed against each other.

## Building, running and checking

Running `./a.out` prints `Hello, world!` on stdout and the intermediate form after each stage on stderr. If it is given a path as its first argument, it also writes its final machine code there.

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

## Running generated code

Every entry that generates machine code runs it the same way: the code is copied into an `mmap`ed buffer, the buffer is made executable with `mprotect`, and the entry's own `main.c` calls it in-process. No entry writes an ELF file or invokes an assembler or linker, so the only thing that differs between entries is the compilation design.

- The generated hello world is raw `write` syscalls followed by `ret`, with no libc calls. It returns to `main` instead of exiting.
- The string lives in the same buffer as the code and is reached with a RIP-relative `lea`, so there is no data section or relocation to model.
- ISO C forbids casting the `mmap` result straight to a function pointer, and `-Wpedantic` rejects it, so entries `memcpy` the pointer into a function-pointer variable instead. They also define `_DEFAULT_SOURCE` at the top, because strict `-std=c17` hides `mmap`.
- Each entry that generates machine code writes its final code bytes to the path it is given, which ends up as `dumps/code.bin`. It can be disassembled with `objdump -D -b binary -m i386:x86-64`. This replaces the object file you would get from a conventional toolchain.

`puc-lua` is an interpreter and generates no machine code, so it runs its bytecode in its own dispatch loop instead and has no `code.bin`.

## When is an entry done

A distilled entry is a toy version of a real design, so "done" has to mean more than "it prints the right text". It is done when all of these hold:

- `scripts/check-entry.sh` passes, which means the output is correct and the committed dumps are up to date.
- Every stage that defines the design is present, in the same order as in the real compiler. For example, GCC lowers from GIMPLE to RTL, and LuaJIT records a trace, builds trace IR and then emits machine code.
- Its `FIDELITY.md` is filled in. Each stage is mapped to the real compiler's stage name and a location in the real source, and every deliberate deviation is listed with a reason.
- A review pass has been done against the real source and, where the real tool can print its own intermediate forms, against those dumps. Useful tools are `clang -emit-llvm` with `-mllvm -print-after-all`, `gcc -fdump-tree-all` and `-fdump-rtl-all`, `luac -l -l`, and LuaJIT's `-jbc` and `-jdump`. Output will not be identical, but the stages, their order and the shape of the data should match.

A reviewer, human or LLM, is given those references and checks specific claims. Asking a model from memory whether a design matches is not enough, because it can misremember how the real compiler works. An LLM is useful for the mechanical checks, such as filling in the stage mapping, spotting missing stages and confirming that the listed deviations match the code. Whether the remaining deviations are acceptable is a human decision.

A research entry has no real counterpart to check against, so it is done when `scripts/check-entry.sh` passes and its `README.md` states the idea, the distilled entries it builds on and how it differs from them.

## Examples

Hello world will make most designs look alike, because the differences show up with control flow and with values that live across instructions. Later examples can form a ladder: arithmetic, a loop, a branch, a function call. Each rung is added to every entry, the same way a Rosetta Code task is.

Examples are checked for correct output, not timed. Designs can still be compared with numbers that do not need a stopwatch, such as emitted instruction count, code size in bytes, or the number of IR nodes at each stage.

## Status

Just started. Nothing here runs yet.

## Non-goals

- Measuring or optimizing performance. The compilers stay tiny, so speed numbers would say little about the real designs.
- Competing with the real compilers on coverage.
- Parsing a real language.
- Being a production tool. These are specimens, not products.

## License

[MIT](LICENSE)
