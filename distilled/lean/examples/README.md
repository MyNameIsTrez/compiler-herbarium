# Examples of lean

Each directory holds one small program over lists, pairs or search trees. `main.c` has no parser, so the same program is written out as IR builders in the `examples` table of `main.c`; the two must agree.

- `program.lua` is the same program in Lua. It is the oracle for the printed result only: the reference counts are checked by the interpreter of `main.c` itself, which fails on a leak or a use after free.
- `expected.txt` is the output of `lua5.4 program.lua`. Regenerate it with the command in `FIDELITY.md`, never by hand.

Lists print as `[1,2,3]`, trees as `(left key right)` with `.` for a leaf, and pairs as `(a, b)`. The list `[1,2,3]` is the input unless the table says otherwise.

| Example | What it shows |
|---------|---------------|
| `append` | `xs` is borrowed, `ys` is stored in the result and stays owned |
| `borrow-live` | `length` borrows the list, which is still live afterwards, so there is no `inc` and no `dec` |
| `bump` | The cell is reset only in the branch that rebuilds it, while two other branches still read it |
| `discard` | An owned result that nobody uses is released with a `dec` right after the call |
| `dup` | A value that is stored twice gets one `inc` |
| `filter` | One branch reuses the cell, the other releases it with `del` |
| `headlen` | `length` reads the list that is then reused, so the reset moves after the call |
| `insert` | A tree update reuses the node on the path and keeps the untouched subtree |
| `length` | The parameter is inferred as borrowed, so no `inc` or `dec` appears |
| `map-inc` | Each cons cell is reset and reused for the result, in place |
| `map-shared` | The input list is still alive, so every reset finds a shared cell and falls back to a fresh allocation |
| `reverse` | The accumulator version reuses every cell of the input |
| `self-append` | One list passed as a borrowed and as an owned argument of the same call |
| `swap` | Two fields of a node exchange places, so `reuse` has to write both |
| `taillen` | A list is released between the reset and the reuse |
| `uncons` | A pair never reuses a cons cell, even though both have two fields |
| `zipadd` | The projections of a second list sit between the projections of the reset cell and the reset |
