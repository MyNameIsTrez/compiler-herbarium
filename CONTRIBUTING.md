# Contributing

Thanks for taking an interest in compiler-herbarium.

## Pull requests are not accepted

Pull requests are restricted to collaborators in this repo's settings, so you will not be able to open one. Every entry has to stay tiny, uniform and checked against the real design, and keeping that consistent is easier when one place makes the changes.

## Open an issue with a lead instead

What helps most is a pointer to something interesting. Please open an issue with a link, for example:

- **A paper** about a compilation technique, an IR, a register allocator, a JIT design or a code generator.
- **A repository** of a compiler or interpreter that has a distinctive design worth distilling. Only open-source designs can become an entry, because the real source is needed to distill one.
- **A talk, blog post or documentation page** that explains a design clearly.
- **A combination idea**, such as "this IR with that backend", or a design that mixes two entries.

A good issue has:

1. The link.
2. One or two sentences on why it is interesting, and what makes the design different from the entries that already exist.
3. If you know it, which existing entry it relates to, or which part of a design the idea would replace.

Please check "What earns an entry" in the [README](README.md) first. A language with trivial semantics, or a compiler that only wraps an existing back end, will be recorded as declined.

It does not need to be polished. A link and a sentence is enough.

Bug reports are welcome as issues too. Please include the entry, the example and the command you ran.

## Where leads go

Leads are recorded in [RESEARCH.md](RESEARCH.md). They then end up in `original/` as new experiments, or in `distilled/` as new toy versions of real compilers. See the [README](README.md) for what each directory holds and when an entry counts as done.

## The usual starting examples

Every entry has its own `examples/` directory. Most entries that compile or interpret programs start from these five, which go from a string to a recursive function. The reference copy is [`distilled/puc-lua/examples/`](distilled/puc-lua/examples/): copy it into a new entry and adapt what the entry needs. Each directory holds a `program.lua`, the definition, and an `expected.txt`, the output of real Lua 5.4.

| Example | What it adds | Output |
|---------|--------------|--------|
| `hello` | Printing a string | `Hello, world!` |
| `arithmetic` | Integer `+`, `-` and `*` with constants | `14` |
| `branch` | Locals, a comparison and an `if` with an `else` | `5` |
| `loop` | A `while` loop that updates locals | `55` |
| `call` | A recursive function with an argument and a return value | `120` |

The programs only use integers, `local` variables, `+`, `-`, `*`, `<`, `if`, `while`, local functions with one argument, `return` and `print`. `print` is only called with a string literal or a non-negative integer, so entries never need to print negative numbers. An entry does not parse them: it hardcodes its own translation.

These are a convention, not a requirement. An entry may use other examples, for example the kernels of `distilled/gso/examples/`, or add examples for the part of the design it is about, as long as its "Regenerating expected.txt" section says how each `expected.txt` is produced.
