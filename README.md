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
  dumps/        the intermediate form after each stage
```

A shared harness runs every entry's output and checks that it prints `Hello, world!`, so the entries stay honest and comparable.

## Beyond hello world

Hello world will make most designs look alike, because the differences show up with control flow and with values that live across instructions. Later tasks can form a ladder: arithmetic, a loop, a branch, a function call. Each rung is added to every entry, the same way a Rosetta Code task is.

## Status

Just started. Nothing here runs yet.

## Non-goals

- Competing with the real compilers on speed or coverage.
- Parsing a real language.
- Being a production tool. These are specimens, not products.
