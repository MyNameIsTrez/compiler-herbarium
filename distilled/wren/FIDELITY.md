# Fidelity: wren

Real design being mimicked: the Wren virtual machine and its bytecode compiler (`src/vm`), the scripting language by Robert Nystrom.

Reference material used (source tree, docs, papers). Pin every source repository to the full 40-character commit hash that was read, also when you name a tag or a branch, because tags and branches move:

- Source: [wren-lang/wren](https://github.com/wren-lang/wren) at `99d2f0b8fc2686134b32b18166e037639f7e9f2c` (HEAD of the default branch when fetched; single files from `src/vm/`). Files read: `wren_opcodes.h` (the opcode list and its order), `wren_vm.c` (`runInterpreter`: the `LOAD_FRAME` and `STORE_FRAME` macros, the `CALL_n` and `completeCall` code, `LOAD_MODULE_VAR`, `LOOP`, `JUMP_IF`, `RETURN`, `END_MODULE`), `wren_vm.h` (`wrenCallFunction`, `wrenGetClassInline`), `wren_value.h` (`CallFrame`, `wrenAppendCallFrame`), `wren_value.c` (`wrenBindMethod`, `wrenNumToString`), `wren_utils.c` (`wrenSymbolTableEnsure` and `Find`), `wren_core.c` (`fn_call`, `DEF_NUM_INFIX`, `system_writeString`, the `PRIMITIVE` and `FUNCTION_CALL` bindings), `wren_core.wren` (`System.print`), `wren_compiler.c` (`emitOp`, `emitJump`, `patchJump`, `defineVariable`, `loadVariable`, `callSignature`, `callMethod`, `infixOp`, `ifStatement`, `whileStatement`, `startLoop`, `endLoop`, `statement`'s `return`, `finishBody`, `endCompiler`, `variableDefinition`, `wrenCompile`'s module epilogue).
- Not read: the scanner and the parse rules other than those above, classes, fields, `super`, constructors, `Fiber` and its `call`, `yield` and `transfer`, foreign methods, imports, the garbage collector, `wren_debug.c`, the compiler's local variable and upvalue resolution.
- Not built or run: the real VM was not compiled, so its disassembly was not compared with this entry's.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | Scanning and parsing | `wren_compiler.c` (`nextToken`, `parsePrecedence`) | The Wren source of the example, shown as text | The scanner and parser themselves: the source is not read by anything |
| `stage_core` | Core initialization | `wren_core.c` (`wrenInitializeCore`), `wren_value.c` (`wrenBindMethod`) | Core classes with a metaclass for the static methods, primitives bound by name and arity into one symbol table, a method array per class indexed by symbol with holes, the core classes as variables of the module (in the real VM they are variables of the core module) | Every class but `Num`, `Fn` and `System`, and every method but seven. The order of the symbols is my own |
| `stage_compile` | Compilation | `wren_compiler.c` (`wrenCompile`, `emitOp`, `callMethod`, `infixOp`, `ifStatement`, `whileStatement`, `endCompiler`) | The calls the parser makes: intern a method name, emit `CALL_n` with a 16-bit symbol, an operator as a call with one argument, jumps with a placeholder patched afterwards, a loop that jumps back by `LOOP`, a top-level `var` as `STORE_MODULE_VAR` and `POP`, a block as a closure constant plus `CLOSURE`, `END_MODULE` and `RETURN` at the end of a module, `CODE_END` after each function, a function disassembled when it is finished | See below |
| `stage_run` | Execution | `wren_vm.c` (`runInterpreter`, `completeCall`), `wren_vm.h` (`wrenCallFunction`) | One loop for all calls, a fiber with its own value stack and call frames, `frame`, `stack_start`, `ip` and `fn` in locals reloaded when a frame changes, the method found by indexing the class's array, primitives that leave the result in the first argument slot, `Fn.call` as a method type of its own that pushes a frame, `RETURN` storing the result in the callee's first slot | See below |

## Deliberate deviations

- There is no scanner and no parser. Each example has a function that makes the calls the real parser makes, in the order it makes them. The real compiler gets there from tokens by Pratt parsing.
- The compiler does not track stack depth. The real `emitOp` adds a stack effect for each instruction and records the high-water mark in `maxSlots`, which `wrenCallFunction` uses to grow the stack. This entry has a fixed stack and does not print the slot count.
- Only numbers reuse a constant that is already in the function's table. The real `addConstant` reuses any value, through a map from value to index.
- The opcode list is the real list with the opcodes this entry cannot emit left out (the true and false literals, the upvalue, field, `SUPER_n`, `AND`, `OR`, class and import opcodes). The order is kept. `LOAD_LOCAL_n` and `CALL_n` are handled by a range test in this loop, where the real one has a label for each.
- `NULL`, `CODE_END` and the other loads are compiled or listed but never run by the examples, so the loop has no handler for them. `fact`'s body ends in a statement, so the compiler emits `NULL` and `RETURN` after its own `RETURN`, as `finishBody` does, and they are never reached.
- Only `false` is falsy here. Real Wren also treats `null` as false. Only comparisons feed a condition in these examples.
- `System.print(_)` is a primitive that writes the number with `%.14g` (what `wrenNumToString` uses) or the string. The real one is Wren code in `wren_core.wren`, calling `writeObject_` and `writeString_`.
- Only the root fiber exists. A real fiber also has a caller, a state and an error, and `Fiber.call`, `yield` and `transfer` switch fibers inside the same loop. None of that is here, since no example needs it. `RETURN` of the last frame ends the run.
- There are no runtime errors: a missing method, a wrong argument type, a wrong arity (`checkArity` for `Fn.call`) and a stack overflow are not checked. The real loop checks that the symbol is inside the method array and not a hole.
- A closure has no upvalues, and `CLOSURE` is not followed by an `isLocal` and an index per upvalue. Nothing in these examples captures a local, because the variables are module variables.
- A module variable is created the first time its name is used, in the order of use. The real compiler also allows a use before the definition (inside `fact`'s body, `fact` is not defined yet) by giving the variable a placeholder that the definition later fills. Here the variable table is only a list of names.
- There is no garbage collector. Objects are kept in a list and freed at exit.
- Classes have no superclass. In `wrenInitializeCore` the real `Object` and `Class` are wired together with `Object metaclass`, and every other class inherits their methods, such as `==(_)` and `toString`.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/wren/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
