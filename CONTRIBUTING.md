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

It does not need to be polished. A link and a sentence is enough.

Bug reports are welcome as issues too. Please include the entry, the example and the command you ran.

## Where leads go

Leads are recorded in [RESEARCH.md](RESEARCH.md). They then end up in `research/` as new experiments, or in `distilled/` as new toy versions of real compilers. See the [README](README.md) for what each directory holds and when an entry counts as done.
