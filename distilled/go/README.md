# go

A cut-down copy of the escape analysis of the Go compiler: a graph of locations whose edges are assignments weighted by dereference counts, solved to decide which allocations may stay on the stack.

## The idea

Go lets you write `&x` and return it. The compiler decides afterwards whether `x` lives on the stack or the heap.

- **Locations and weighted edges.** Every variable and every allocation site is a location. An assignment `dst = src` is an edge from `src` to `dst` with a weight: the number of dereferences minus the number of address-of operations. `p = &x` has weight -1, `p = *q` has weight 1.
- **A walk from each root.** For each location the solver finds the least number of dereferences with which every other location reaches it (a Bellman-Ford walk, clamped at 0). If a location is reached with weight -1, its address flows to the root.
- **When that matters.** The address escapes if the root is the heap, or if the root outlives the location: it belongs to an outer loop, or it is a result of the function.
- **Parameter tags.** Functions are analyzed callees first. What a callee does with each parameter is summarized in a tag (leaks to the heap or not), and a caller uses the tag instead of looking inside.
- **Closures.** A closure captures a variable by reference, which is an address-of edge, so a variable that a closure outlives moves to the heap.

The Lua examples are shown as the Go program a Go implementation of Lua would be: every value an `any`, so every stored number is a box. The verdicts of `stage_escape` and a run that really allocates on the stack or the heap, and dies on a dangling stack pointer, are in `dumps/<example>/stages.txt`.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source. The short version: typed trees built by hand instead of a front end, the heap as the only leak, closures always by reference, a fixpoint over all edges instead of a worklist, and one parameter and result per function.
