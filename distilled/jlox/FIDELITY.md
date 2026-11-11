# Fidelity: jlox

Real design being mimicked: jlox, the tree-walking interpreter in part II of "Crafting Interpreters" by Robert Nystrom.

Reference material used:

- Source: [munificent/craftinginterpreters](https://github.com/munificent/craftinginterpreters) at `4a840f70f69c6ddd17cfef4f6964f8e1bcd8c3d4` (HEAD of the default branch when fetched, shallow clone; files from `java/com/craftinginterpreters/lox/`). Files read: `Resolver.java` (all visit methods, `resolveLocal`, `resolveFunction`, `declare`, `define`), `Environment.java` (all), `LoxFunction.java` (all), `Return.java`, `Interpreter.java` (`resolve`, `executeBlock`, the visit methods for blocks, functions, if, print, return, var, while, assign, call and variable, and `lookUpVariable`), `AstPrinter.java` (the first part). The files carry the book's snippet markers, so the final form of each function was picked out.
- Not read: `Scanner.java`, `Parser.java`, `Lox.java`, `LoxClass.java`, `LoxInstance.java`, `Expr.java` and `Stmt.java` (generated), the class, `this` and `super` parts of the resolver and interpreter, the unary, logical, get and set parts of the interpreter.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | Scanning and parsing | `Scanner.java`, `Parser.java`; the printed form follows `AstPrinter.java` | A syntax tree of statement and expression nodes, built before anything runs | The scanner and the parser: each example's tree is built by hand. The printer puts one statement per line and prints expression statements with a leading `;`, as `visitExpressionStmt` does |
| `stage_resolve` | Resolving | `Resolver.java` (`resolve`, `beginScope`, `endScope`, `declare`, `resolveLocal`, `resolveFunction`), `Interpreter.resolve` | A stack of scopes, one per block and per function body; a distance counted from the innermost scope; the distances stored in a side table keyed by tree node, not in the tree; the initializer of `var` resolved before the name is declared; a function's name declared before its body is resolved; parameters declared in the function's own scope | See below |
| `stage_run` | Interpretation | `Interpreter.java` (`execute`, `executeBlock`, `lookUpVariable`, the visit methods), `Environment.java` (`getAt`, `assignAt`, `ancestor`), `LoxFunction.java` (`call`) | A chain of environments with a parent link, a new one per block and per call; reads and writes `distance` links up; unresolved names looked up in the globals; closures that keep the environment current at `fun`; a call environment whose parent is the closure; `return` as a non-local jump caught by the call | See below |

## Deliberate deviations

- The scanner and the parser are replaced by tree-building functions. They are the part every interpreter shares, and the entry is about what happens after the tree exists.
- The language is the subset the examples use: `print`, `var` with an initializer, expression statements, blocks, `if`/`else`, `while`, `fun` with exactly one parameter, `return` with a value, `+ - * <`, assignment, calls with exactly one argument, numbers and strings. There are no unary operators, equality, logical operators, `for`, classes, `this` or `super`.
- There are no errors. The real resolver reports `var a = a;`, a duplicate name in one scope, and `return` at the top level, and the interpreter reports type errors, arity mismatches and undefined variables. None of those can happen in these programs.
- `declare` and `define` are one step. The real resolver first marks a name as declared but not defined (`false` in the scope map) so that it can reject a read of the variable inside its own initializer, and then marks it defined. Without the error the marker has no use.
- Scopes and environments are small arrays searched with `strcmp`, in place of a `Stack<Map<String, Boolean>>` and a `HashMap<String, Object>`. They hold at most four names.
- Assigning to a global is not implemented. The real `visitAssignExpr` falls back to `globals.assign` when there is no distance. No example assigns to a global.
- `return` uses `setjmp` and `longjmp`, where jlox throws a `Return` exception (which has the stack trace turned off) and `LoxFunction.call` catches it. The call restores the current environment by hand, where `executeBlock` does it in a `finally` clause. A function that ends without `return` would yield the previous return value instead of `nil`.
- Values are bools, numbers and strings, and function objects. There is no `nil`, and a condition is read as a bool, because only comparisons feed a condition here. `print` writes `%g` for numbers.
- There is no native `clock`, so the globals start empty. The memory of all environments and nodes is freed at exit, since a garbage collector would not teach anything here (Java provides one to jlox).
- The trace of variable accesses and calls in `stage_run` is this entry's own. jlox prints nothing while it runs (checked by running it).

## Comparison with the real tool

The real jlox was compiled with `javac` from the same commit (a scratch clone, since removed) and run on each example translated by hand to Lox, for example `{ var sum = 0; var i = 1; while (i < 11) { sum = sum + i; i = i + 1; } print sum; }` for `loop`.

- Command: `java -cp out com.craftinginterpreters.lox.Lox <example>.lox`, plus a ten-line harness in the `lox` package that runs `Scanner`, `Parser` and `AstPrinter` on the file.
- Matches: all five examples print exactly `expected.txt`, and jlox prints nothing on stderr while it runs. The trees have the same shape as this entry's: for `call`, `(fun fact(n) (if (< n 2.0) (block (return 1.0)))(return (* n (call fact  (- n 1.0)))))`, and expression statements start with `;`, as here.
- Differs: `AstPrinter` prints the whole tree on one line, writes numbers as `1.0` and variable declarations as `(var sum = 0.0)`, and shows parentheses as a `group` node, which the hand-built trees here leave out. The resolver distances and the run trace in this entry's dumps cannot be compared, because the real jlox does not print them. The distances were checked by reading `resolveLocal` only.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/jlox/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
