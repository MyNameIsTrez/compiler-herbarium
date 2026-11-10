# naive

The simplest compiler this repo can have. It is the baseline that every other entry can be compared with.

## The idea

The input is a stack machine IR. Each IR instruction is expanded into a fixed run of x86-64 bytes, with nothing remembered between instructions:

- Every operand lives on the machine stack, and every local lives in a stack slot.
- The registers are scratch only, and no value stays in a register from one IR instruction to the next.
- Jumps, calls and the string are emitted with a blank 32-bit offset, and a second pass fills them in once every instruction has an address.

There is no analysis, no optimization and no register allocation. This is how a baseline template JIT works at its simplest, without the inline caches and type feedback that real ones add.

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
gcc research/naive/main.c
./a.out call
```
