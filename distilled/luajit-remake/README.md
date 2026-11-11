# luajit-remake

A cut-down copy-and-patch baseline JIT, after LuaJIT Remake and its VM generator Deegen.

## The idea

A copy-and-patch compiler has no code generator in the usual sense. Ahead of time, the machine code for each bytecode is compiled once and cut into a *stencil*: a template of bytes in which every value that is only known later (a stack slot, a constant, an address, a jump target) is a *hole*. At run time the JIT does two things per bytecode, copy the stencil into the code buffer and patch values into its holes. There is no instruction selection, no register allocation and no assembler in the JIT.

In LuaJIT Remake the stencils come out of LLVM: Deegen compiles the C++ implementation of each bytecode and reads the relocations of the object file. This entry writes the stencils by hand instead, as byte arrays with a table of holes, and keeps the part that runs in the JIT.

- **Stencils and holes.** `ADD` is one 68-byte template. Which slots it reads and writes are 32-bit displacement holes, and the jump to the slow path is a relative hole.
- **A shared slow path.** Each fast path checks that the operands are integers and jumps to a slow path otherwise. The fast paths are laid out together and the slow path sits after them, off the hot path.
- **No jump to the next bytecode.** A stencil cannot know what follows it, so it ends with a jump to the next bytecode. The JIT places the next bytecode right after, so it cuts those five bytes off before copying.
- **Two passes.** The layout is fixed first, so a jump can name a bytecode that comes later, and then the holes are filled.

The entry is checked against the five programs in [`examples/`](examples/). The stages, the offset of every bytecode and the value patched into every hole are in `dumps/<example>/stages.txt`, and the JIT'ed code is in `dumps/<example>/code.bin`, with the host addresses zeroed.

## Stages

- `stage_bytecode`: the bytecode of each function
- `stage_stencils`: the stencils in use, with their bytes and holes
- `stage_layout`: where each fast path and the slow path land
- `stage_patch`: copy each stencil and fill its holes
- `stage_export`: write `code.bin`, make the code executable and run it

## What it leaves out

There is no compiler, no interpreter, no inline caches, no upvalues, no garbage collector and no optimizing tier. Values are integers, strings and functions, and the slow path only reports an error.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source, which parts of it were read, and what was dropped.
