# Fidelity: go

Real design being mimicked: the escape analysis of the Go compiler (`cmd/compile/internal/escape`).

Reference material used:

- Source: [golang/go](https://github.com/golang/go) at `26a3c1f26e117eba697ef4ad87dd487aea46e58d` (HEAD of the default branch when fetched; files from `src/cmd/compile/internal/`). Files read: `escape/graph.go` (location, edge, hole, `flow`, `teeHole`), `escape/solve.go` (`walkAll`, `walkOne`, `outlives`), `escape/leaks.go` (the leak encoding), `escape/call.go` (`call`, `tagHole`, `mergedTagHole`), `escape/escape.go` (`Batch`, `initFunc`, `walkFunc`, `flowClosure`, `finish`, `paramTag`, `reportLeaks`), `escape/expr.go` (address-of, dereference, conversion to interface, slice literals, closures), `escape/assign.go`, `escape/stmt.go` (loops, return, declarations).
- Not read: `ir.VisitFuncsBottomUp`, `ir.ContainsClosure`, the reassignment oracle, `escape/utils.go` in detail, `escape/alias.go`. `ssa/compile.go` and `ssa/regalloc.go` in this tree are small stubs, so the SSA passes and register allocation were not read and are not in this entry.
- Not built or run: the real compiler was not compiled, so its `-m` output was not compared with this entry's.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | Parsing | `syntax`, `noder` | The Lua source of the example, shown as text | The parser: the source is not read by anything |
| `stage_build` | Type checking and IR construction | `typecheck`, `noder` | The program as a typed tree and its Go listing | See below |
| `stage_escape` | Escape analysis | `escape/escape.go` (`Batch`), `escape/solve.go`, `escape/call.go` | Locations, edges weighted by derefs, a walk from every root, `outlives` with loop depth, parameter tags carried from callees to callers, closures as address-of edges, boxes and slice literals as allocation sites, the verdict messages | See below |
| `stage_run` | Code generation and the runtime | `walk`, `ssagen`, the runtime allocator | Each allocation placed on the stack or the heap as decided, and a check that no stack pointer outlives its frame | The real stages; this is a small interpreter of my own |

## Deliberate deviations

- The typed trees are built by hand per example. There is no front end and no inlining. The Lua-as-`any` lowering is my own.
- Batches (the strongly connected components of the call graph, callees first) are listed by hand. The real compiler computes them with `ir.VisitFuncsBottomUp`.
- Leaks track only the heap. The real bitset also has a bit per result and a bit for mutations and calls. Results are treated as escaping here, and parameters are all pointer-carrying, so no tag is skipped for scalars.
- A closure always captures by reference. The real compiler captures by value a variable that is never reassigned and is small, and tracks `reassigned` and `captured`.
- The solver relaxes all edges until nothing changes. The real one keeps a queue of locations and skips edges whose source already escapes.
- `outlives` has no rule for closure containment, and `flow` has no fast path and no self-assignment filter.
- One parameter and one result per function. No address-of expressions in the source, no maps, `append`, `go`, `defer` or `unsafe`. A `return` inside a loop is not supported.
- `Println` stands in for `fmt.Println`: it stores its first element in a global, so its parameter's content leaks.
- Messages have no source positions.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
