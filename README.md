# compiler-herbarium

A collection of heavily cut-down compilers and interpreters, each written from scratch to mimic the design of a well-known real one. Think of it as Rosetta Code for compiler architecture: one fixed task, many implementations, laid out so you can compare them side by side.

The repo is meant as a learning resource for people, and as a reference that LLMs can read when researching new ways to do compilation.

## The idea

Every entry compiles (or interprets) the same tiny program, effectively:

```lua
print("Hello, world!")
```

There is no tokenizer and no parser. Each entry starts from a hardcoded data structure in the form its real counterpart would use internally, such as a Sea of Nodes graph, an SSA control flow graph or a bytecode prototype. From there it runs the stages that are characteristic of the design and produces either machine code or a running interpreter loop.

What distinguishes the designs lives in the middle of the pipeline, so each entry **dumps its intermediate form after every stage**. The dumps are the main output. Reading them in order shows how a design thinks, and diffing them across entries shows where the designs differ.

## Language and platform

Every compiler and interpreter in this repo is written in C17, so differences between entries come from the design rather than from the implementation language.

This repo is Linux only. Entries may rely on Linux system calls and Linux-specific memory mapping, and no effort is made to support other operating systems.

## Planned entries

| Entry | Mimics | What it should show |
|-------|--------|---------------------|
| `sea-of-nodes` | Cliff Click's Sea of Nodes IR (see [SeaOfNodes/Simple](https://github.com/SeaOfNodes/Simple)) | Graph IR with floating nodes, scheduling, instruction selection |
| `clang` | Clang and LLVM | AST-free lowering to SSA, mem2reg style cleanup, instruction selection, register allocation |
| `gcc` | GCC | GIMPLE, RTL, and the pass pipeline |
| `puc-lua` | The reference Lua interpreter | Register bytecode and the dispatch loop |
| `luajit` | LuaJIT | Interpreter, hot trace recording, trace IR, machine code |

More designs can be added over time. The list is a plan, not a promise.

## Layout of an entry

Each entry follows the same shape, so the same file names can be opened in different directories and compared:

```
<entry>/
  README.md     what the design is, what this version leaves out
  input.*       the hardcoded program in the design's native IR
  ...           the stages, one source file per stage where practical
  dumps/        the intermediate form after each stage, and code.bin
```

A shared harness runs every entry's output and checks that it prints `Hello, world!`, so the entries stay honest and comparable.

## Running generated code

Every entry that generates machine code runs it the same way: the code is copied into an `mmap`ed buffer, the buffer is made executable with `mprotect`, and the harness calls it in-process. No entry writes an ELF file or invokes an assembler or linker, so the only thing that differs between entries is the compilation design.

- The generated hello world is raw `write` syscalls followed by `ret`, with no libc calls. It returns to the harness instead of exiting.
- The string lives in the same buffer as the code and is reached with a RIP-relative `lea`, so there is no data section or relocation to model.
- The harness forks one process per entry, so a crashing entry cannot take the harness down.
- Each entry also writes its final code bytes to `dumps/code.bin`, which can be disassembled with `objdump -D -b binary -m i386:x86-64`. This replaces the object file you would get from a conventional toolchain.

`puc-lua` is an interpreter and generates no machine code, so it runs its bytecode in its own dispatch loop instead.

## Beyond hello world

Hello world will make most designs look alike, because the differences show up with control flow and with values that live across instructions. Later tasks can form a ladder: arithmetic, a loop, a branch, a function call. Each rung is added to every entry, the same way a Rosetta Code task is.

## Status

Just started. Nothing here runs yet.

## Non-goals

- Competing with the real compilers on speed or coverage.
- Parsing a real language.
- Being a production tool. These are specimens, not products.
