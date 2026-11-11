# Fidelity: ghc

Real design being mimicked: GHC's STG language and the STG machine, as the compiler lowers STG to Cmm and as the runtime system executes it.

Reference material used:

- Source: [ghc/ghc](https://github.com/ghc/ghc) (the GitHub mirror of [gitlab.haskell.org/ghc/ghc](https://gitlab.haskell.org/ghc/ghc)) at `19ff36f2aede0ca2e554016ff22c71e73aa9803b` (the commit the abbreviated hash `19ff36f2` names, downloaded file by file with no clone). Files read: `compiler/GHC/Stg/Syntax.hs` (the expression and right-hand side types, `UpdateFlag`), `compiler/GHC/Stg/Make.hs` (`mkStgRhs`, `mkStgRhsCon_maybe`, `mkTopStgRhs`), `compiler/GHC/CoreToStg.hs` (`mkStgApp`, `coreToStgLet`, `coreToStgArgs`), the header comment and the module list of `compiler/GHC/Stg/FVs.hs`, `compiler/GHC/StgToCmm/Expr.hs` (`cgExpr`, `cgLneBinds` and the let-no-escape functions, the header of `cgCase`, `cgIdApp`), `compiler/GHC/StgToCmm/Bind.hs` (`thunkCode`, `setupUpdate`, `pushUpdateFrame`, the function entry in `closureCodeBody`), `rts/Updates.cmm` (`stg_upd_frame`), `rts/Apply.cmm` (the first function, `stg_ap_0_fast`).
- Further reading, not used as a source: the Haskell wiki page "I know kung fu: learning STG by example" ([Wayback Machine capture of 3 March 2018](https://web.archive.org/web/20180303050346/https://ghc.haskell.org/trac/ghc/wiki/Commentary/Compiler/GeneratedCode)), supplied by the repo owner. It describes a GHC of that time compiled to x86, so it may not match the current architecture. Nothing in this entry was taken from it or checked against it.
- Not read: the rest of `Stg/FVs.hs` (the free-variable computation itself, so how this entry computes the sets is its own), the bodies of `cgCase`, `cgConApp` and `cgOpApp`, `rts/Apply.cmm` apart from its first function (PAPs, over-saturated and under-saturated calls), `rts/StgMiscClosures.cmm`, `rts/Eval` and the garbage collector, `GHC.Stg.Lift*`, `GHC.Stg.CSE`, `GHC.Stg.Unarise`, `GHC.Stg.Pipeline`, and everything in `GHC.Core` and `GHC.Core.Opt` (the simplifier, demand analysis, CorePrep).

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_source` | The front end, the Core simplifier, CorePrep and the first half of CoreToStg | `GHC/CoreToStg.hs` (`mkStgApp`, `coreToStgArgs`, `coreToStgLet`); the language is `GHC/Stg/Syntax.hs` | An STG program: atoms as the only arguments, `let` for allocation, `case` for evaluation, saturated constructor and primop applications, `let-no-escape` for a join point (`coreToStgLet` makes it from a Core join binding) | The whole front end: each example is built by hand as unoptimized STG would look. `Int` is the boxed `I#` and a comparison gives `1#` or `0#`, as in GHC. See below |
| `stage_flags` | CoreToStg's choice of right-hand side | `GHC/Stg/Make.hs` (`mkStgRhs`, `mkStgRhsCon_maybe`, `mkTopStgRhs`) | A zero-parameter binding whose body is a constructor application becomes a constructor closure (`StgRhsCon`); a join binding is `JumpedTo` (`\j`); a binding with parameters is `ReEntrant` (`\r`); any other is `Updatable` (`\u`). A top-level function is `\r`. | `SingleEntry` (`\s`), which needs the demand analyser to know a thunk is entered once. Every thunk here is `\u`. Cost-centre stacks, ticks and types |
| `stage_fvs` | Free-variable annotation | `GHC/Stg/FVs.hs` (described in its header; the algorithm was not read) | Each closure records the variables it captures; top-level names are not captured; a recursive group's own names count as free in it; join points are not captured | Dependency sorting of let groups (`depSortWithAnnotStgPgm`), unboxed and void arguments, and the ordering and layout of the captured variables in the closure. Sets are in order of first use |
| `stage_run` | The code `GHC.StgToCmm` generates and the RTS code it jumps to | `StgToCmm/Expr.hs` (`cgIdApp`, `cgCase`, `cgLneBinds`), `StgToCmm/Bind.hs` (`thunkCode`, `setupUpdate`, `pushUpdateFrame`), `rts/Updates.cmm` (`stg_upd_frame`), `rts/Apply.cmm` (`stg_ap_0_fast`) | Four of the ways `cgIdApp` treats a variable: return it if it is a value (`ReturnIt`), enter it if it is a thunk (`EnterIt`), call a function with all its arguments (`DirectEntry`), jump to a join point (`JumpToIt`); a thunk pushes an update frame and its closure's free variables are its whole environment; `case` pushes a continuation and evaluates the scrutinee; an update frame overwrites the thunk with an indirection; following an indirection on entry; allocation by `let`; a join point allocates no closure and a jump leaves the stack alone | See below |

## Deliberate deviations

- Haskell, the type checker and everything in Core are left out. The simplifier is the part of GHC that is shared with no one else, but it is not this entry's subject, and the repo's rules leave out generic optimizations. The examples are written as STG with no optimization: every use of a variable is a new `case` on it, there is no strictness analysis and no unboxing, so arguments and accumulators are thunks. This is how the author wrote them, not something checked against GHC's `-O0` output. Real GHC at `-O` would turn `fact` and `loop` into loops over `Int#` with no thunks at all.
- The language is the subset the examples use: `let` and `let-no-escape` with one or more bindings, `case` with a constructor alternative, literal alternatives and a default, function and thunk applications, one constructor (`I#` with one field), and the primops `+#`, `-#`, `*#`, `<#`, `putInt#` and `putStr#`. There are no other data types, so no tag checks or multi-field constructors.
- The program's output uses `putInt#` and `putStr#` as primops that print at once, in place of GHC's `IO` and its state token. Each is just the last expression of the path that prints.
- There is no partial application (`PAP`), no over-saturated call and no `stg_ap_*` generic apply code, so the arity of a callee is trusted to equal the number of arguments. In the real machine `cgIdApp` also chooses `SlowCall` (unknown function) and falls back to generic apply for anything that is not an exact match. No example calls a function with the wrong number of arguments or an unknown function.
- A thunk is not overwritten with a black hole when it is entered. The real code (`blackHoleIt` in `thunkCode`, lazy or eager) does this so a thunk that depends on itself is detected and so the garbage collector does not keep its free variables alive. Without a collector or a loop in the data, neither matters here.
- The update frame writes an indirection and the thunk keeps its old captured variables in the array. The real update overwrites the thunk's header with `stg_IND` and the garbage collector later shorts the indirection out.
- The heap and the stack are fixed arrays with no bounds checks, there is no garbage collector, and values are addresses and numbers, not tagged pointers. The pointer tag that real GHC puts in a pointer to say a closure is evaluated (and which constructor) is not modelled: this entry always looks at the closure.
- The entry interprets STG. GHC lowers it to Cmm, with a heap check at the start of each function and thunk, an explicit stack of words and registers (`Sp`, `Hp`, `R1`), and info tables with entry code. None of that is here, so there is no `code.bin`.
- A let-no-escape binding is jumped to by name from a table of join points, with the environment saved when the `let` ran. The real code jumps to a block of the same function, with the free variables already in registers or stack slots.
- Every `case` pushes its frame and then enters the scrutinee, also when the scrutinee is already a value, which the machine here handles by returning it to the frame it just pushed. Whether the real code skips work in that case was not read (`cgCase` was not read past its header).

## Comparison with the real tool

None. GHC was not installed, and building it would need hours and more memory than this laptop has. The shape of the entry's output (update frames pushed on thunk entry, case continuations, thunks updated with indirections) was compared by reading `rts/Updates.cmm`, `thunkCode` and `cgIdApp`. `ghc -ddump-stg-final` would print each example's real STG, and could be compared with `stage_flags` and `stage_fvs`.

- Command: none run.

## Regenerating expected.txt

Every `examples/<name>/expected.txt` is the output of real Lua 5.4 on that example's `program.lua`, so it does not rest on this entry. Never edit one by hand. From the repository root:

```
for d in distilled/ghc/examples/*/; do lua5.4 "$d/program.lua" > "$d/expected.txt"; done
```

`scripts/check-expected.sh` re-runs the oracle for every entry and fails if a file differs.

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| | | | |
