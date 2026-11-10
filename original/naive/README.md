# naive

The simplest compiler this repo can have. It is the baseline that every other entry can be compared with.

## The idea

The input is a stack machine IR. Each IR instruction is expanded into a fixed run of x86-64 bytes, with nothing remembered between instructions:

- Every operand lives on the machine stack, and every local lives in a stack slot.
- The registers are scratch only, and no value stays in a register from one IR instruction to the next.
- Jumps, calls and the string are emitted with a blank 32-bit offset, and a second pass fills them in once every instruction has an address.

There is no analysis, no optimization and no register allocation. It is closest in spirit to a baseline compiler such as V8's Sparkplug, but it is not a distillation of one. See the next section for what matches and what does not.

## Relation to V8's Sparkplug

Checked on 2026-11-10 against the [Sparkplug blog post](https://v8.dev/blog/sparkplug) and V8's `src/baseline/baseline-compiler.cc` and `baseline-compiler.h` on the main branch.

What matches:

- **No IR of its own.** Sparkplug compiles bytecode directly to machine code, as `naive` does. The blog says it "doesn't generate any intermediate representation (IR) like most compilers do".
- **A linear walk with one case per instruction.** `BaselineCompiler::GenerateCode` iterates over the bytecode and calls `VisitSingleBytecode` for each one, which is the same shape as the `switch` in `stage_emit`.
- **A label per jump target, patched later.** Sparkplug keeps a `labels_` entry per bytecode offset and binds it when it reaches that offset. `naive` records the offsets itself and patches them in `stage_patch`, which has the same effect.
- **One backend per architecture.** The blog says there is no architecture-independent stage, so every port is written separately. `naive` targets x86-64 only.

What differs, and why `naive` is not a distillation of it:

- **Input.** Sparkplug reads Ignition's register bytecode, which has an accumulator. `naive` reads a stack machine IR.
- **Where values live.** Sparkplug keeps the interpreter's frame layout, with the registers as frame slots and the accumulator in a machine register, so that on-stack replacement and deoptimization work unchanged. `naive` keeps every operand on the machine stack and has no interpreter to be compatible with.
- **Builtins.** Most Sparkplug bytecodes become a call into a builtin shared with the interpreter, since JavaScript semantics are too large to emit inline. `naive` has no runtime, so every operation is a few inline instructions.
- **A pre-pass.** `GenerateCode` first walks the bytecode to mark loop headers and exception handlers as jump targets, so the walk is not strictly a single pass. The blog describes it as a single linear pass, and `naive` does the same walk plus a separate patching step.
- **Feedback.** Sparkplug uses a feedback vector gathered by the interpreter. `naive` has no profiling.

So `naive` shares Sparkplug's central trick, expanding each instruction with no IR in between, but it is a different, smaller thing. It is an original entry for that reason.

## Stages

| Stage | What it does |
|-------|--------------|
| `stage_select` | Picks the hardcoded IR program for the example |
| `stage_emit` | Expands each IR instruction into bytes and records where each one starts |
| `stage_patch` | Fills in the jump, call and string offsets |
| `stage_run` | Maps the bytes as executable memory and calls them |

## What it leaves out

- Anything a smarter design would do with the stack machine: keeping values in registers, removing push and pop pairs, folding constants, inlining the call.
- Negative numbers in `PRINT_INT`, which is not needed by the examples.
- More than 15 locals in a function, and more than one function argument.

## What it shows

Every other design in this repo should produce shorter or faster code than this one, and the committed dumps make the difference visible. For `call`, the recursive factorial, this entry emits a push and pop around almost every operation, where an optimizing design would keep `n` in a register.

## Building

```
gcc original/naive/main.c
./a.out call
```
