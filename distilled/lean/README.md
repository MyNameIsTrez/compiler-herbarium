# lean

A cut-down copy of the reference counting passes of the Lean 4 compiler: where a function can reuse the memory of a value that it is about to throw away, which parameters can be borrowed instead of owned, where the `inc` and `dec` instructions go, and how a `reset`/`reuse` pair turns into an in-place update when the cell is not shared.

## The idea

Lean has no garbage collector. Every heap object carries a count, and the compiler decides where the count changes, so the pure functional program `map f xs` can run as a loop that updates a list in place when nobody else holds it.

- **Reset and reuse.** When a function takes a constructor apart and builds a new one of the same shape, the compiler inserts `reset x` after the last read of `x` and `reuse` in place of the new allocation. The pass looks for the last use on every path, and refuses to reset a cell that is still read, or that is stored in the new value.
- **Borrowing.** A parameter that is only read, never stored or returned or passed to an owning parameter, is marked `@&` and the caller keeps its reference. This is a fixpoint over all functions, and a self tail call stays a tail call because an owned argument makes its parameter owned.
- **Explicit counting.** A backward pass over the function body tracks which variables are live and which are only borrowed. An owned variable that is used again gets an `inc`, one that is dead gets a `dec`. A projection of a live or borrowed object is a derived borrow and costs nothing unless it is consumed.
- **Expansion.** `reset` and `reuse` have no run time meaning of their own. The last pass tests `isShared` on the cell. If it is exclusive, the fields that were not read are released and the cell itself becomes the token, so `reuse` writes the new fields in place and skips the `inc` that the projections would have needed. If it is shared, the incs run, the cell is released once, and `reuse` allocates.
- **The machine.** `stage_run` interprets the result on a heap that counts every reference, and fails if an object is freed twice, used after it was freed, or still alive at the end.

The dumps in `dumps/<example>/stages.txt` show the function bodies after each pass, and the number of allocations, incs, decs and cells found exclusive or shared at run time.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source. The short version: first-order functions over lists, pairs and search trees, no type checker and no boxing, one constructor per reuse, and no relaxed reuse or coalescing. No Lean binary was run, so the shape of each pass is compared with the source and not with a real dump.
