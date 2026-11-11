# Fidelity: virgil

Real design being mimicked: the whole-program middle end of Aeneas, the compiler of the Virgil language, namely the compile-time initializer run, the reachability analysis, the partial specialization of generic code and the normalization of tuples to scalars.

Reference material used:

- Source: [titzer/virgil](https://github.com/titzer/virgil) at `0465a6c2b04c4691963ef6c01d0299345efc38ad` (shallow sparse clone of `aeneas` and `doc`). Files read: `doc/impl/Compiler.md`; in `aeneas/src/ir/Reachability.v3` lines 1 to 760 (the flags and facts for classes, methods and fields, `analyze`, the handling of records, fields, live classes and virtual methods, the operator cases, and how fields and methods are marked); `aeneas/src/ir/PartialSpecialization.v3` (the `Specializer`: the signature of an operator, `putField`, `putCallConv`, the initial split by operators, `splitBasedOnCalls` and `processCalls`); the headers and the comments at the top of `aeneas/src/ir/Normalization.v3`, `VariantSolver.v3` and `PackingSolver.v3`; the headers and phases of `aeneas/src/main/Compiler.v3` (parse, verify, init, reachability, emit).
- Not read: the bodies of `Normalization.v3`, `VariantSolver.v3` and `PackingSolver.v3`, the SSA normalizer and optimizer, the verifier, the parser, and the backends.
- Built and run: nothing. Virgil was not built, and the examples were not compared against Aeneas.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | the program after parsing and verification | `Compiler.v3` (parse and verify phases, not read in depth) | Classes with single inheritance, methods, globals, generic functions with a type parameter, and an initializer that builds the heap | No parser or type checker: the example is built by C functions that make a typed tree. One type parameter per function; a type argument is a concrete type or the function's own parameter, which means no type is built while running |
| `stage_init` | the `init` phase | `Compiler.v3`, `doc/impl/Compiler.md` | The initializer runs on an interpreter at compile time and leaves a heap of objects and the values of the globals | The interpreter is the same evaluator that runs `main` at the end. No variants, closures, arrays, strings or components. Objects hold up to eight scalars |
| `stage_reach` | reachability analysis | `Reachability.v3` | A worklist from `main` over functions and instances. Per field: read, written, the values stored (none, one known, many). A virtual call records (class, method name) and is resolved against every live subclass, now and when a class becomes live later. A class becomes live when live code allocates it or when a live heap object is an instance of it. The heap is traced from the globals that live code reads, through the fields that live code reads. The call edges and the polymorphic operations of each instance are recorded for the next stage | Facts are plain flags, not the full lattice of the real analysis. A value counts as known only when it is a literal or comes from the heap; a written field is simply not constant. No arrays, closures, variants, method pointers, or the stricter treatment of components and exports |
| `stage_specialize` | partial specialization of polymorphic code | `PartialSpecialization.v3` | The signature of an instance is the list of its operations whose code depends on the type argument: tuple creation and projection with their scalar counts, `new T` with the class, and the scalar kinds (integer or reference) of the parameters and result of polymorphic calls. Instances with the same signature start in one group, and a group splits when two members call different groups, repeated until the number of groups does not change | The signature is a flat list of integers and no operator-specific hashing. Only one type parameter and no generic classes. Calls that are not through the type parameter do not enter the signature |
| `stage_normalize` | normalization of tuples, objects and calls | `Normalization.v3` (header read, body not read) | Deleted fields, constant fields (their reads become the constant), flat slot offsets for the rest, tuples as lists of scalars, the signature of each copy in scalar kinds, and the live heap rewritten to the new layout | No variant solver, no packing solver (no packing of fields into spare bits), no boxing of values of unknown size. The layout is one slot per scalar |
| `stage_run` | the compiled program | `emit` phase (not modelled) | `main` runs on the new layout; every generic call prints the copy it reaches | There is no code generation: the evaluator reads the normalized layout directly |

## Deliberate deviations

- No parser or type checker. A program is built by C functions that make a tree of expressions with their types already attached.
- Fields are never inherited: a class has its own fields, and the parent only supplies methods and the subtype relation. The real language gives a subclass the fields of its parent.
- A field is constant when all of its values are one known number and live code never writes it. The real analysis tracks a larger set of cases.
- The specializer's signature and the order of its rounds follow the shape of the real code (initial split by operators, then refinement by callee groups) but the exact contents of a real signature were not compared entry by entry.
- Only int, class and pair types exist. There are no floating point numbers, bytes, arrays, enums, variants or function values.
- The object heap is a fixed array of 16 objects and an object holds at most 8 scalars; the dump notation (`#3`, `const(5)`, `copy name#k`) is this entry's own.
- Virgil's own statements (loops, locals, assignment) are replaced by expressions (`if`, `seq`, global and field assignment).

## Comparison with the real tool

None run: Virgil is not built here. The oracle is real Lua running the same program, which checks only the result printed by `main`. The shape of the stages was compared with the files listed above, and the dumps were not compared with the output of the real compiler.

## Regenerating expected.txt

The oracle is real Lua 5.4, which runs the same program that `main.c` builds. Each `program.lua` prints the result of `main`.

```
for d in distilled/virgil/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
