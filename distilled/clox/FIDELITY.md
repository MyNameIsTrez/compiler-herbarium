# Fidelity: clox

Real design being mimicked: clox, the bytecode interpreter in part III of "Crafting Interpreters" by Robert Nystrom.

Reference material used:

- Source: [munificent/craftinginterpreters](https://github.com/munificent/craftinginterpreters) at `4a840f70f69c6ddd17cfef4f6964f8e1bcd8c3d4` (HEAD of the default branch when fetched; single files from `c/`). Files read: `compiler.c` (almost all), `vm.c` (`call`, `callValue`, `captureUpvalue`, `closeUpvalues`, `run` cases for the jumps, calls, closures and arithmetic), `object.c` (`newClosure`, `newFunction`, `newUpvalue`), `debug.c` (disassembly format). The files carry the book's snippet markers, so the final form of each function was picked out.
- Not read: `scanner.c`, `memory.c` (garbage collector), `table.c` (hash table and string interning), `value.c`, the class and method parts of the compiler and VM.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | Scanning | `scanner.c` (`scanToken`) | The token types and their texts | The scanner itself: each example's tokens are written down by hand. Line numbers are not kept |
| `stage_compile` | Single-pass compilation | `compiler.c` (`compile`, `parsePrecedence`, `declaration`, `statement`, `endCompiler`) | Pratt parsing with a rules table, code generation inside the parser, `emitJump`/`patchJump`/`emitLoop`, locals as stack slots, `isCaptured`, upvalues that `OP_CLOSURE` lists, a compiler struct per function, disassembly of each function when it is finished (`DEBUG_PRINT_CODE`) | See below |
| `stage_run` | Execution | `vm.c` (`run`, `call`, `captureUpvalue`, `closeUpvalues`) | One dispatch loop for all calls, call frames with a `slots` window into one value stack, open upvalues pointing into the stack and closed on `OP_CLOSE_UPVALUE` and `OP_RETURN`, the stack trace before each instruction (`DEBUG_TRACE_EXECUTION`) | See below |

## Deliberate deviations

- The scanner is replaced by hand-written token arrays. In clox the scanner is a separate pass that the parser pulls tokens from, and it is the least interesting part of the design.
- The grammar is the subset the examples use: `print`, `var`, `fun` with exactly one parameter, `if`/`else`, `while`, `return` with a value, blocks, `+ - * <`, calls with exactly one argument, grouping, numbers and strings. There are no globals, unary operators, equality, `and`/`or`, `for`, classes or methods. Every name is a local or an upvalue.
- There are no compile errors or runtime errors. The tokens are well formed, so `consume` never fails, and the VM does not check types, arity or stack depth.
- Assignment is spotted by looking one token ahead in `variable`. The real parser passes a `canAssign` flag through every parse function so that `a + b = c` is rejected. Only locals can be assigned: there is no `OP_SET_UPVALUE`.
- A local is declared with its final depth at once. The real compiler first gives it depth -1 and sets it when the initializer is done, to reject `var a = a;`.
- Upvalues capture only a local of the directly enclosing function. The real `resolveUpvalue` recurses through the enclosing functions and records an `isLocal` flag per upvalue, so a closure can capture another closure's upvalue. Here `OP_CLOSURE` is followed by one slot byte per upvalue, and a function that names the same variable twice gets two upvalues, where `addUpvalue` reuses one.
- Open upvalues are a plain list with the newest in front. The real list is sorted by stack address, and `captureUpvalue` returns an existing upvalue for the same slot, so two closures share a variable. Here each closure captures its own.
- Values are bools, nil, doubles and objects, as in clox, but the object list is only used to free everything at exit. There is no garbage collector, no string interning and no NaN boxing.
- Chunks have fixed-size arrays for code and constants instead of growable ones, and carry no line numbers.
- `is_falsey` only handles bools, because only comparisons feed a condition in these examples.
- `print` writes `%g` for numbers, as `printValue` does.

## Comparison with the real tool

Real clox was built from the same commit with `gcc -O1 c/*.c -lm`, in a scratch clone since removed. `common.h` was edited in the copy (the four `#undef DEBUG_*` lines and the two GC debug defines removed) so that `DEBUG_PRINT_CODE` and `DEBUG_TRACE_EXECUTION` stay on. Each example was translated by hand to Lox: the same program as in `examples/`, wrapped in a block for `branch`, `loop` and `call`, so that its variables are locals as here.

- Command: `./clox <example>.lox`
- Matches: all five examples print exactly `expected.txt`. After the differences below are normalized away, the disassembly of every function and every line of the stack trace are identical to the stage dumps of this entry, for all five examples.
- Differs: real clox prints a source line column in the disassembly (`0000    1 OP_CONSTANT`, then `|`), and names a function `== fact ==` where this entry writes `== <fn fact> ==`. `OP_CLOSURE` is followed in real clox by two bytes per upvalue (an `isLocal` flag and the index), so the offsets after it are one larger there. Real clox prints the program's output between the trace lines, and this entry sends it to stdout and the dump to stderr.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/clox/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
