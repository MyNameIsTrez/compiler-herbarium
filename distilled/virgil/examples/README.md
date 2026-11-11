# Examples of virgil

Each directory holds one small program. `main.c` has no parser, so the program is built by a function of the same name in the `examples` table of `main.c`; `program.lua` is the same program in Lua.

- `program.lua` is the oracle. It runs the program in real Lua 5.4 and prints the result of `main`, which is what `main.c` prints. It checks the result only, not which functions, fields and copies the compiler keeps.
- `expected.txt` is the output of `lua5.4 program.lua`. Regenerate it with the command in `FIDELITY.md`, never by hand.

| Example | What it shows |
|---------|---------------|
| `const-field` | A field that is only ever one value and never written is replaced by it (`scale`), a field that is written stays in the object (`count`), and a field that is written but never read is deleted (`log`) |
| `generics` | Each of `rt`, `id`, `first` and `dup` is instantiated for an integer, two classes and a pair, and gets three copies: one for integers, one shared by both classes, one for pairs. `fresh<A>` and `fresh<B>` start in one group but call different copies of `alloc`, so the second round of refinement splits them |
| `shapes` | Virtual calls resolved by reachability. `Tri` is never allocated by live code, so its method is dropped, and the `Tri` objects in the heap are reachable only through a field nobody reads. `Circle.r` is always 5 and disappears, `tag` is never read and disappears, `Dot` inherits its method, and a `Square` shared by two nodes is reached twice |
| `tuple-field` | A field of pair type takes two slots in the object, a pair-typed field that is never read (`origin`) frees both |
