# virgil

A cut-down copy of the whole-program middle end of Aeneas, the compiler of the Virgil language: it runs the program's initializers at compile time, finds what the program can really use, shares one copy between the instantiations of a generic function that do not need separate code, and lays out objects without the fields nobody reads.

## The idea

Aeneas compiles a whole program at once, so it can decide things a separate compiler cannot.

- **The initializers run first.** Virgil has no static constructors that run at start-up. Aeneas interprets the initializers of the globals while compiling and keeps the heap they build, objects and all. The heap is part of the program from then on.
- **Reachability starts at `main`.** A worklist of functions finds the methods that can be called. A virtual call does not mark every override: it records the receiver's class and method name, and the override of every class that is (or later becomes) live and inherits from that class is added. A class is live when live code allocates it or when a live heap object is an instance of it.
- **Fields are judged by use.** A field that no live code reads is deleted from its class, and the object in the heap does not hold it. An object that is reachable only through deleted fields disappears from the heap. A field that is only ever given one value, and is never written after construction, is replaced by that value.
- **Generic functions are shared when they can be.** `id<A>` and `id<B>` need one copy if the code does the same thing to a reference of either type. The specializer gives every instantiation a signature made of the operations whose code depends on the type argument (how many scalars a tuple has, which class `new T` makes, whether a parameter is an integer or a reference). Instantiations with equal signatures start in one group. A group then splits when two of its members call different groups, repeated until the number of groups stops growing (partition refinement over the call graph).
- **Normalization flattens.** After the choices above, every type is a list of scalars: a tuple becomes its parts, a field of tuple type becomes several slots, a function takes and returns scalars.

`dumps/<example>/stages.txt` shows the program, the heap made by the initializer, the live functions, classes, fields and objects, every round of the partition refinement with the resulting copies, the new layout of each class and signature of each copy, and, while `main` runs, which copy every generic call goes to.

## Fidelity

See [FIDELITY.md](FIDELITY.md) for the mapping to the real source. The short version: the program is built by C functions that make a small typed tree (no parser, no type checker), the language has ints, classes with single inheritance, pairs, and generic functions over one type parameter, and there are no variants, closures, arrays, SSA graphs or code generation. Virgil was not built, and the oracle is real Lua 5.4 running the same program.
