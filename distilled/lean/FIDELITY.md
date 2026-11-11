# Fidelity: lean

Real design being mimicked: the reference counting passes of the Lean 4 compiler: `insertResetReuse`, `inferBorrow`, `explicitRc` and `expandResetReuse`.

Reference material used:

- Source: [leanprover/lean4](https://github.com/leanprover/lean4) at `6959fb4c1061c713e09e2fd6a9ca4adf616a1290` (default branch, shallow sparse clone of `src/Lean/Compiler/IR`, `src/Lean/Compiler/LCNF`, `src/include/lean` and `src/runtime`, never built). Files read in full: `src/Lean/Compiler/LCNF/ResetReuse.lean`, `InferBorrow.lean`, `ExplicitRC.lean`, `ExpandResetReuse.lean`. Read in part: `PropagateBorrow.lean` (the first 60 lines, the ownedness lattice for borrow annotations), `CoalesceRC.lean` (the comment that describes it), `Passes.lean` (the order of the passes), `src/include/lean/lean.h` (`lean_inc_ref`, `lean_dec_ref`, `lean_is_exclusive`, `lean_free_small_object`).
- Not read: `lean_dec_ref_cold` and the rest of `src/runtime/object.cpp`, `LiveVars.lean`, `ExplicitBoxing.lean`, the part of `PropagateBorrow.lean` after its first 60 lines, and everything before `toImpure` (the LCNF simplifier, specialization, closure conversion).
- Built and run: nothing. Lean is not installed, so there is no real dump to compare with.

The passes live in `LCNF` in this commit. Older versions had them in `Lean/Compiler/IR` with the same names.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | the code after `toImpure` | `Passes.lean` | First-order functions in the shape the later passes see: `let` of a literal, constructor, projection or call, `case` with one alternative per constructor, `ret`. Constructor fields are scalar or object typed | Everything before it. The program is written with builders in `main.c` instead of being parsed and compiled. No join points, closures, partial applications, arrays or constants in the source |
| `stage_reset_reuse` | `insertResetReuse` | `ResetReuse.lean` | The three procedures R, D and S: for each constructor alternative R tries to reset the scrutinee, D finds the code after which the variable is dead, S replaces a constructor of the right shape that follows by `reuse`. The rule that a variable stored in the new cell cannot be recycled, and that an owned argument to a call ends its life | The relaxed second run, the `alreadyFound` set, the rule for reset targets that are derived from another variable, `collectResetTargets`. A cell is reused only for the same constructor, so there is no `setTag` and no scalar fields |
| `stage_borrow` | `inferBorrow` | `InferBorrow.lean` | Start with every object parameter borrowed and make it owned until nothing changes: a parameter becomes owned if it is returned, stored in a constructor, reset, or passed to an owned parameter, and a self tail call keeps the parameter owned so the call stays a tail call. The functions of an example are one group | Calling a function by variable, partial application, user annotations and `PropagateBorrow`, `@[export]` and `@[extern]` functions, the split into strongly connected components |
| `stage_rc` | `explicitRc` | `ExplicitRC.lean` | A backward walk with the live variables and the borrowed ones (`Live{vars, borrows}`), derived borrows (a projection of a live or borrowed object), `inc` before a consuming use of a live or borrowed variable, `dec` after a call that only borrowed it, `dec` of an unused result, the `dec`s at the start of an alternative for variables that the alternative does not use, and the `inc` there for a variable that was borrowed before the `case` and is owned inside, scalars skipped (`inc_ref`/`dec_ref` when a value is known to be a pointer) | `succeedingSets`, the size of the object in a `dec`, join points, the cases for arrays and thunks, constants. A projection is registered as a child only for its direct parent |
| `stage_expand` | `expandResetReuse` | `ExpandResetReuse.lean` | `reset` becomes an `isShared` test, with a fast path (the cell is exclusive: the `inc`s of the projections before the reset are removed, the fields nobody read are released, and `reuse` becomes `set` of the changed fields) and a slow path (the incs, one `dec` of the cell, `reuse` allocates). Both rejoin at a join point that takes the token. A `dec` of the token on a path that does not reuse it becomes `del` | The second `reuse` expansion for a reset whose token is in a join point, the `isShared` shortcut for objects of unknown shape, `setTag`, scalar and `usize` fields. `CoalesceRC`, which merges neighbouring `inc` and `dec`, is not kept |
| `stage_run` | the runtime | `lean.h`, `object.cpp` (not read) | A heap of cells with a count, boxed scalars, `inc`, `dec` that frees the children when the count reaches zero, `isShared`, `del`, and a check of the result | Threads, the small object allocator, big objects, multi-threaded counts (negative counts), persistent objects |

## Deliberate deviations

- No parser, type checker or boxing: the source is a small IR written with builders, since the passes start after those stages. A `Nat` is a boxed scalar in an object field, and nil and leaf are scalars, so they never get an `inc` or a `dec`.
- A cell is reused only for the same constructor. The real compiler also reuses a cell for another constructor of the same type and size and fixes the tag with `setTag`. Dropping it removes the second flag and the rule about types, and no example needs it.
- An `inc` or `dec` has no count. The real instruction `inc x n` becomes `n` separate `inc x` here, so a variable consumed three times has two of them. Because of this `erase_proj_incs` only has to remove one inc per projection.
- The expansion assumes what the earlier passes produce: the `inc` of a projected field comes right after its projection, a projection of the reset cell is not repeated, and only one reset is in flight on a path, so an expansion does not meet another reset's `reuse`. The real pass checks more cases.
- A possible problem in the real source, not run and not confirmed: `eraseProjIncFor` collects the instructions it keeps in `keep`, adds the projection and then an `inc` with a count, and appends `keep.reverse` at the end. For a count above one the `inc` would come before its projection. The entry never has a count above one.
- Derived borrows are registered only for projections of a variable, and projections of projections are not followed.
- The functions of an example are one group for `inferBorrow`, since none of the examples has a call that leaves the group.
- The dump notation (`block_0 (x : obj) :=`, `reuse x in ctor_1[cons]`, `inc_ref`, `@&`) is this entry's own; it follows the IR printer in spirit and not in letter.

## Comparison with the real tool

None run: Lean is not installed, so no output of the real compiler was compared. The shape of each pass was compared with the source files listed above, and the behavior in the dumps (for example that `map-inc` reuses the three cells of an exclusive list and allocates three new cells for a shared one) follows from reading them, not from a real dump.

## Regenerating expected.txt

The oracle is real Lua 5.4: each `program.lua` computes the same list, tree or pair as the IR example and prints it in the same text format. It checks the printed result only. The counts and the absence of leaks and double frees are checked by `stage_run` itself, which exits with an error when one is wrong.

```
for d in distilled/lean/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
