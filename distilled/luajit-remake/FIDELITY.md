# Fidelity: luajit-remake

Real design being mimicked: LuaJIT Remake (luajit-remake/luajit-remake) and its VM generator Deegen, whose baseline JIT is a copy-and-patch compiler.

Reference material used:

- Source: [luajit-remake/luajit-remake](https://github.com/luajit-remake/luajit-remake) at `a1275ae480c7766d5b41df2d9edd6b1c637090ad`, a sparse checkout of `deegen/` only. Files read, in part: `deegen_baseline_jit_impl_creator.cpp` (`BaselineJitImplCreator::DoLowering`), `deegen_stencil_creator.cpp` (the relocation handling and `PrintStencilCodegenLogicImpl`, which turns a stencil into code that copies and patches it), `deegen_stencil_runtime_constant_insertion_pass.h` and `.cpp` (the runtime-constant expressions that become holes), `deegen_stencil_reserved_placeholder_ords.h`, and `deegen_jit_codegen_logic_creator.cpp` (the fast path, slow path and data section base addresses of the generated codegen function).
- Not read: the Lua bytecode definitions and the Lua runtime (outside `deegen/`, never checked out), the register pinning scheme and calling convention of the stencils, inline caches and call inline caches, the DFG optimizing tier, and the full text of the copy-and-patch and Deegen papers. Only their abstracts and author lists were checked.
- Paper (abstract and author list only): Haoran Xu and Fredrik Kjolstad, [Copy-and-Patch Compilation](https://fredrikbk.com/publications/copy-and-patch.pdf), OOPSLA 2021.

## Stage mapping

| Stage in this entry | Real stage name | Where it lives in the real source | Kept | Dropped or simplified |
|---------------------|-----------------|-----------------------------------|------|-----------------------|
| `stage_bytecode` | The Lua bytecode that Deegen's generated VM compiles source into | Not read (the bytecode definitions and the parser were not read) | A register bytecode: operands are slots of the frame | All of the compiler. The bytecode is this entry's own small set (`LOADI`, `LOADK`, `MOVE`, `ADD`, `SUB`, `MUL`, `JNLT`, `JMP`, `CALL`, `RET1`, `RET0`) written by hand, not the Lua 5.1 bytecode of the real VM |
| `stage_stencils` | Stencil extraction at build time: compile each bytecode's implementation to an object file, then turn its machine code and relocations into a stencil | `deegen_stencil_creator.cpp`, `deegen_stencil_lowering_pass.cpp` (only the parts listed above were read) | A stencil is bytes plus a table of holes. Each hole has a place, a width and the value to patch in. The kinds are 32-bit displacements and immediates, 64-bit addresses and 32-bit relative jumps, which match the relocation types `R_X86_64_32`, `32S`, `64`, `PC32` and `PLT32` that the real code accepts | The stencils are hand-assembled byte arrays, not LLVM output, so there is no object-file parsing, no relocation addends, no register patching and no shared constant data section. Each stencil is hand-written for a fixed register use (`rbx` as frame base, `rax`, `rcx`, `rdx` as scratch) instead of the real calling convention |
| `stage_layout` | Choosing where each fast path and slow path goes, before codegen | `deegen_jit_codegen_logic_creator.cpp` (fast path, slow path and data section addresses) | Fast paths are laid out contiguously in bytecode order and the slow paths are placed apart from them. The 5-byte `jmp` that ends a stencil is cut off, because the next bytecode follows immediately, which is what `PrintStencilCodegenLogicImpl` does when the fallthrough placeholder is present at the end of the code. Offsets are fixed before any hole is written, so jumps can name later bytecodes | One slow path per function, shared by every guard, where the real design has a slow path stencil per bytecode (and per inline cache). No data section. No alignment of the data section |
| `stage_patch` | The generated codegen function: copy the stencil bytes, then store each placeholder value into its hole | `PrintStencilCodegenLogicImpl` in `deegen_stencil_creator.cpp`, and `CPRuntimeConstantNodeBase` in `deegen_stencil_runtime_constant_insertion_pass.cpp` | The two steps at run time: copy the bytes, then write the value of each hole, with relative holes written as destination minus the end of the hole, for slots, constants, immediates and jump targets | The real values are expressions over runtime constants (add, subtract, multiply, extensions, truncation) that are folded into one C++ snippet per bytecode, and some are patched by adding to the bytes already present. Here each hole takes one value computed directly from the instruction |
| `stage_export` | Making the code executable and entering it | Not read | `mmap`, `mprotect`, and a call into the code | The real VM keeps one growing code region and enters code through its own calling convention. code.bin has the host addresses zeroed, which the real design has no equivalent of |

## Deliberate deviations

- Values are integers, strings and functions. The fast path of `ADD`, `SUB`, `MUL` and `JNLT` checks that both operands are integers and jumps to the slow path otherwise, as a guarded fast path does in the real baseline JIT. The slow path here only reports a type error, because no example needs more, and it is never executed. Integer arithmetic wraps and does not check for overflow.
- There is no interpreter. LuaJIT Remake runs bytecode in a generated interpreter first and tiers up to the baseline JIT, and this entry compiles every function before running it. The Kind column of the README says Interpreter because the real design is a VM, but this entry has only the JIT half.
- A call loads the callee's entry point from the function object and calls it with the C calling convention. The real design has call inline caches that patch direct calls. Upvalues and closures are not modeled: `fact` finds itself through a constant.
- The real stencils get their calling convention from LLVM and pin VM state in registers, and they chain by tail calls. These are plain `call` and `ret` with a frame base in `rbx`. This part of the real design was not read.
- Only the baseline JIT is mimicked. LuaJIT Remake has a second, optimizing tier (a DFG JIT, seen in the file names of `deegen/`), which this entry leaves out and which was not read.

## Comparison with the real tool

The real stencils are produced by building the LuaJIT Remake repository with LLVM, which was not done. There is no dump of the real tool to compare with. The hole kinds and the final-jump elision are compared with the source only, as listed above.

- Command: none
- Matches: the structure of a stencil (bytes, holes, final fallthrough jump removed) and of the two-step patcher, as read in `deegen_stencil_creator.cpp`
- Differs: see the deviations and the dropped column above

## Review

| Date | Reviewer | References given to the reviewer | Result |
|------|----------|----------------------------------|--------|
| 2026-11-10 | The agent that wrote the entry (self review, no independent reviewer) | The source files listed above | Compared with the code read, as above. The parts listed as not read were not checked. An independent review is still open |
