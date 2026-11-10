/*
 * luajit-interpreter: a cut-down version of the hand-written assembler interpreter of LuaJIT 2.1.
 *
 * LuaJIT's interpreter is not C. It is one block of x86-64 assembly (src/vm_x64.dasc), written
 * for a fixed assignment of registers: the bytecode pointer, the frame base, the constant base
 * and the dispatch table each live in a register for the whole run. Every bytecode handler
 * ends by decoding the next instruction word itself and jumping through the dispatch table, so
 * there is no central loop. The table is also how the VM changes mode: while a trace is being
 * recorded every entry points to one routine that calls the recorder first, and a loop or call
 * instruction that counts how often it runs has a twin that does not count. A counter that
 * underflows calls into C, and a trace that fails a guard comes back through an exit handler.
 *
 * This entry generates that interpreter as x86-64 machine code when it starts, with a small
 * assembler, and runs the examples on it. The VM is then the code in code.bin. For the one
 * hot loop it also makes a trace out of templates (not LuaJIT's IR and register allocator,
 * which is distilled/luajit) so that the dispatch switching, JLOOP and the exit handler all run.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the form after each stage on stderr. code.bin
 *   receives the code area: the interpreter, and the trace if one was made.
 *
 * Stages, in the order they run:
 *   stage_bytecode   the bytecode of each function, in LuaJIT's instruction format
 *   stage_assemble   generate the interpreter as x86-64 machine code
 *   stage_dispatch   fill the dispatch tables and the hot counters
 *   stage_interpret  run the program on the generated interpreter
 *   stage_record     while recording, the dispatch table sends every instruction to a C hook
 *   stage_trace      when recording reaches the loop again, make the trace and patch JLOOP in
 *   stage_exit       a guard failed: the exit handler saves the registers and calls into C
 */
#define _DEFAULT_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

// GCOVR_EXCL_START: this entry handles only what the examples need, and stops on the rest
static void require(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "unsupported: %s\n", what);
        exit(1);
    }
}
// GCOVR_EXCL_STOP

/* ------------------------------------------------------------------------------------------
 * Values and bytecode. A value is 8 bytes. The top 17 bits hold a type tag, as in LuaJIT's
 * 64-bit mode: a pointer has the tag shifted left by 47 above it, and an integer is the tag
 * LJ_TISNUM above its 32 bits. A tag check is a shift right by 47 and a compare.
 * ---------------------------------------------------------------------------------------- */

#define LJ_TSTR (~4u)
#define LJ_TFUNC (~8u)
#define LJ_TISNUM (~13u)
#define TAG(t, v) (((uint64_t)(t) << 47) | (v))
#define GCVMASK ((UINT64_C(1) << 47) - 1)

#define FRAME_C 1u /* low bits of the saved pc in a frame: 0 is a Lua frame */
#define FRAME_TYPE 3

/* The instruction words are LuaJIT's: the opcode in the low byte, then A, and then either D in
 * the upper 16 bits or C and B in the two bytes above A. The order of the opcodes matters,
 * since the VM adds a constant to an opcode to get its counting or patched twin. */
enum bc {
    BC_ISGE, BC_KSTR, BC_KSHORT, BC_KFUNC, BC_MOV, BC_ADDVN, BC_SUBVN, BC_ADDVV, BC_MULVV,
    BC_CALL, BC_RET0, BC_RET1, BC_LOOP, BC_ILOOP, BC_JLOOP, BC_JMP, BC_FUNCF, BC_IFUNCF,
    BC_FUNCC, BC__MAX
};
_Static_assert(BC_LOOP + 1 == BC_ILOOP && BC_LOOP + 2 == BC_JLOOP, "LOOP, ILOOP, JLOOP");
_Static_assert(BC_FUNCF + 1 == BC_IFUNCF, "FUNCF, IFUNCF");

#define BCBIAS_J 0x8000
#define BCINS_AD(o, a, d) ((uint32_t)(o) | (uint32_t)(a) << 8 | (uint32_t)(d) << 16)
#define BCINS_ABC(o, a, b, c) ((uint32_t)(o) | (uint32_t)(a) << 8 | (uint32_t)(c) << 16 | (uint32_t)(b) << 24)
#define BCINS_AJ(o, a, j) BCINS_AD(o, a, (uint32_t)((j) + BCBIAS_J))
#define J(here, target) ((target) - ((here) + 1)) /* a jump is relative to the next instruction */

#define bc_op(i) ((i) & 0xff)
#define bc_a(i) (((i) >> 8) & 0xff)
#define bc_b(i) ((i) >> 24)
#define bc_c(i) (((i) >> 16) & 0xff)
#define bc_d(i) ((i) >> 16)
#define bc_j(i) ((int)bc_d(i) - BCBIAS_J)

static const char *const bc_name[BC__MAX] = {
    "ISGE", "KSTR", "KSHORT", "KFUNC", "MOV", "ADDVN", "SUBVN", "ADDVV", "MULVV", "CALL",
    "RET0", "RET1", "LOOP", "ILOOP", "JLOOP", "JMP", "FUNCF", "IFUNCF", "FUNCC",
};

/* A function object starts with a GC header word, then the pc of its first instruction. A Lua
 * function points into its prototype, and a C function points to a one-instruction bytecode
 * FUNCC, so a call dispatches on the first word the same way for both. */
struct func {
    uint64_t gch;
    const uint32_t *pc;
    void (*cf)(const uint64_t *args);
};

/* A prototype keeps the bytecode last, and the header before it, so that the interpreter finds
 * everything from the pc: the constant pointer sits at a fixed distance before the first
 * instruction. KBASE points between the two constant arrays. Object constants count down from
 * it (index ~i), numbers count up. */
#define MAXBC 16
struct proto {
    uint64_t kgc[3];
    uint64_t knum[1];
    const uint64_t *k;
    uint8_t n;
    uint8_t framesize;
    uint32_t bc[MAXBC];
};
#define PC2PROTO_K ((int)offsetof(struct proto, k) - (int)offsetof(struct proto, bc))

static void rt_print(const uint64_t *args)
{
    if ((int64_t)args[0] >> 47 == (int32_t)LJ_TSTR) {
        printf("%s\n", (const char *)(uintptr_t)(args[0] & GCVMASK));
    } else {
        printf("%d\n", (int32_t)(uint32_t)args[0]);
    }
}

static const uint32_t bc_cfunc[1] = { BCINS_AD(BC_FUNCC, 0, 0) };
static struct func print_fn = { 0, bc_cfunc, rt_print };
static struct func fact_fn = { 0, NULL, NULL };
static struct func main_fn = { 0, NULL, NULL };

/* The array is aligned so that the position of each instruction, and with it the hot counter it
 * hashes to, is the same in every build. */
static _Alignas(256) struct proto protos[2];
static int nprotos;

static struct proto *new_proto(struct func *fn, uint8_t framesize)
{
    struct proto *p = &protos[nprotos++];
    p->k = p->knum;
    p->framesize = framesize;
    p->bc[p->n++] = BCINS_AD(BC_FUNCF, framesize, 0);
    fn->pc = p->bc;
    return p;
}

static void add(struct proto *p, uint32_t ins)
{
    p->bc[p->n++] = ins;
}

/* Object constant number i is stored at k[-1 - i]. */
static void set_kgc(struct proto *p, int i, const void *obj, uint32_t tag)
{
    p->kgc[2 - i] = TAG(tag, (uint64_t)(uintptr_t)obj);
}

static const char hello[] = "Hello, world!";

/* These are the bytecodes LuaJIT's parser emits, with three differences, listed in FIDELITY.md:
 * a function constant is loaded with KFUNC (LuaJIT uses GGET, UGET and FNEW), a call has one
 * result (LuaJIT chains CALLM), and nothing is constant folded. The frame of a call has the
 * function in slot A, the saved pc in slot A+1, and the arguments from slot A+2. */
static void example_hello(void)
{
    struct proto *p = new_proto(&main_fn, 3);
    set_kgc(p, 0, &print_fn, LJ_TFUNC);
    set_kgc(p, 1, hello, LJ_TSTR);
    add(p, BCINS_AD(BC_KFUNC, 0, 0));
    add(p, BCINS_AD(BC_KSTR, 2, 1));
    add(p, BCINS_ABC(BC_CALL, 0, 1, 2));
    add(p, BCINS_AD(BC_RET0, 0, 1));
}

static void example_arithmetic(void)
{
    struct proto *p = new_proto(&main_fn, 4);
    set_kgc(p, 0, &print_fn, LJ_TFUNC);
    p->knum[0] = TAG(LJ_TISNUM, 6);
    add(p, BCINS_AD(BC_KFUNC, 0, 0));
    add(p, BCINS_AD(BC_KSHORT, 2, 2));
    add(p, BCINS_AD(BC_KSHORT, 3, 3));
    add(p, BCINS_ABC(BC_ADDVV, 2, 2, 3));
    add(p, BCINS_AD(BC_KSHORT, 3, 4));
    add(p, BCINS_ABC(BC_MULVV, 2, 2, 3));
    add(p, BCINS_ABC(BC_SUBVN, 2, 2, 0));
    add(p, BCINS_ABC(BC_CALL, 0, 1, 2));
    add(p, BCINS_AD(BC_RET0, 0, 1));
}

static void example_branch(void)
{
    struct proto *p = new_proto(&main_fn, 5);
    set_kgc(p, 0, &print_fn, LJ_TFUNC);
    add(p, BCINS_AD(BC_KSHORT, 0, 3));
    add(p, BCINS_AD(BC_KSHORT, 1, 5));
    add(p, BCINS_AD(BC_ISGE, 0, 1));
    add(p, BCINS_AJ(BC_JMP, 2, J(4, 9)));
    add(p, BCINS_AD(BC_KFUNC, 2, 0));
    add(p, BCINS_AD(BC_MOV, 4, 1));
    add(p, BCINS_ABC(BC_CALL, 2, 1, 2));
    add(p, BCINS_AJ(BC_JMP, 4, J(8, 12)));
    add(p, BCINS_AD(BC_KFUNC, 2, 0));
    add(p, BCINS_AD(BC_MOV, 4, 0));
    add(p, BCINS_ABC(BC_CALL, 2, 1, 2));
    add(p, BCINS_AD(BC_RET0, 0, 1));
}

static void example_loop(void)
{
    struct proto *p = new_proto(&main_fn, 5);
    set_kgc(p, 0, &print_fn, LJ_TFUNC);
    p->knum[0] = TAG(LJ_TISNUM, 1);
    add(p, BCINS_AD(BC_KSHORT, 0, 0));
    add(p, BCINS_AD(BC_KSHORT, 1, 1));
    add(p, BCINS_AD(BC_KSHORT, 2, 11));
    add(p, BCINS_AD(BC_ISGE, 1, 2));
    add(p, BCINS_AJ(BC_JMP, 2, J(5, 10)));
    add(p, BCINS_AJ(BC_LOOP, 2, J(6, 10)));
    add(p, BCINS_ABC(BC_ADDVV, 0, 0, 1));
    add(p, BCINS_ABC(BC_ADDVN, 1, 1, 0));
    add(p, BCINS_AJ(BC_JMP, 2, J(9, 3)));
    add(p, BCINS_AD(BC_KFUNC, 2, 0));
    add(p, BCINS_AD(BC_MOV, 4, 0));
    add(p, BCINS_ABC(BC_CALL, 2, 1, 2));
    add(p, BCINS_AD(BC_RET0, 0, 1));
}

static void example_call(void)
{
    struct proto *m = new_proto(&main_fn, 6);
    set_kgc(m, 0, &print_fn, LJ_TFUNC);
    set_kgc(m, 1, &fact_fn, LJ_TFUNC);
    add(m, BCINS_AD(BC_KFUNC, 0, 1));
    add(m, BCINS_AD(BC_KFUNC, 1, 0));
    add(m, BCINS_AD(BC_MOV, 3, 0));
    add(m, BCINS_AD(BC_KSHORT, 5, 5));
    add(m, BCINS_ABC(BC_CALL, 3, 2, 2));
    add(m, BCINS_ABC(BC_CALL, 1, 1, 2));
    add(m, BCINS_AD(BC_RET0, 0, 1));
    struct proto *f = new_proto(&fact_fn, 5);
    set_kgc(f, 0, &fact_fn, LJ_TFUNC);
    f->knum[0] = TAG(LJ_TISNUM, 1);
    add(f, BCINS_AD(BC_KSHORT, 1, 2));
    add(f, BCINS_AD(BC_ISGE, 0, 1));
    add(f, BCINS_AJ(BC_JMP, 1, J(3, 6)));
    add(f, BCINS_AD(BC_KSHORT, 1, 1));
    add(f, BCINS_AD(BC_RET1, 1, 2));
    add(f, BCINS_AD(BC_KFUNC, 1, 0));
    add(f, BCINS_ABC(BC_SUBVN, 3, 0, 0));
    add(f, BCINS_ABC(BC_CALL, 1, 2, 2));
    add(f, BCINS_ABC(BC_MULVV, 1, 0, 1));
    add(f, BCINS_AD(BC_RET1, 1, 2));
    add(f, BCINS_AD(BC_RET0, 0, 1));
}

static const struct {
    const char *name;
    void (*build)(void);
} examples[] = {
    { "hello", example_hello },
    { "arithmetic", example_arithmetic },
    { "branch", example_branch },
    { "loop", example_loop },
    { "call", example_call },
};

/* Where an instruction lives, for the dumps: the function and the index of the instruction. */
static struct proto *proto_of(const uint32_t *ip)
{
    return ip >= protos[1].bc ? &protos[1] : &protos[0];
}

static int bc_index(const uint32_t *ip)
{
    return (int)(ip - proto_of(ip)->bc);
}

static const char *fn_name(const uint32_t *ip)
{
    return proto_of(ip) == &protos[0] ? "main" : "fact";
}

static void print_ins(int idx, uint32_t w)
{
    int op = (int)bc_op(w);
    P("  %04d  %-6s %3u", idx, bc_name[op], bc_a(w));
    if (op == BC_ISGE || op == BC_KSTR || op == BC_KFUNC || op == BC_MOV || op == BC_RET0 ||
        op == BC_RET1) {
        P(" %3u", bc_d(w));
    } else if (op == BC_KSHORT) {
        P(" %3d", (int16_t)bc_d(w));
    } else if (op == BC_JMP || op == BC_LOOP) {
        P(" => %04d", idx + 1 + bc_j(w));
    } else if (op == BC_FUNCF) {
        P(" (frame size %u)", bc_a(w));
    } else {
        P(" %3u %3u", bc_b(w), bc_c(w));
    }
    P("\n");
}

static void stage_bytecode(void)
{
    P("== stage_bytecode: LuaJIT's instruction format, A, B, C and D are fields of the word ==\n");
    for (int i = 0; i < nprotos; i++) {
        const struct proto *p = &protos[i];
        P("function <%s> (%d instructions), integer constant:", i == 0 ? "main" : "fact", p->n);
        P(" %d\n", (int32_t)(uint32_t)p->knum[0]);
        for (int j = 0; j < p->n; j++) {
            print_ins(j, p->bc[j]);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * The VM's state, in one block that the DISPATCH register points to. It holds the dispatch
 * tables, the hot counters and the places the assembly saves things before it calls C, so the
 * generated code reaches all of it with one register and a fixed offset, and the code does not
 * contain a host address.
 * ---------------------------------------------------------------------------------------- */

#define HOTCOUNT_SIZE 64
#define HOTCOUNT_PCMASK ((HOTCOUNT_SIZE - 1) * (int)sizeof(uint16_t))
#define HOTCOUNT_LOOP 2
#define HOTCOUNT_CALL 1
#define HOTLOOP 2 /* 56 in LuaJIT */
#define MAXTRACE 2
#define STACK_SLOTS 64

struct exitstate {
    uint64_t gpr[15]; /* r15 first, rax last: the order the handler pushes them in, backwards */
    uint64_t exitno;
};

struct gg {
    void *dispatch[BC__MAX];
    void *static_disp[BC_FUNCF];
    uint16_t hotcount[HOTCOUNT_SIZE];
    uint64_t *base;     /* the frame base, saved before a call into C (L->base) */
    uint64_t *jit_base; /* the frame base while a trace runs */
    const void *trace_mcode[MAXTRACE];
    void (*cb_hotloop)(const uint32_t *pc);
    void (*cb_record)(const uint32_t *pc);
    void (*cb_hotcall)(const uint32_t *pc);
    const uint32_t *(*cb_exit)(const struct exitstate *ex);
};
static struct gg gg;

#define GG(field) ((int32_t)offsetof(struct gg, field))

/* ------------------------------------------------------------------------------------------
 * The assembler. LuaJIT writes its interpreter with DynASM, which turns assembler text into
 * calls like these. Only the instructions the VM uses are here. A memory operand is a base
 * register, an optional index register with a scale, and a displacement. A branch to a label
 * leaves a 32-bit hole that is filled in when the code is linked.
 * ---------------------------------------------------------------------------------------- */

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

/* The register assignment of vm_x64.dasc (POSIX), under the names it uses. */
#define BASE RDX     /* frame base: slot i is [BASE + i*8] */
#define PC RBX       /* the pc of the next instruction */
#define DISPATCH R14 /* the VM state, and the dispatch table at its start */
#define KBASE R15    /* the constants of the running function */
#define RA RCX       /* the A field of the current instruction */
#define RB RBP       /* the B field, and the opcode (OP) during dispatch */
#define OP RBP
#define RC RAX       /* the C field, or D (RD) */
#define RD RAX
#define ITYPE R11    /* scratch for type checks */

enum { CC_O = 0x0, CC_B = 0x2, CC_NE = 0x5, CC_L = 0xC, CC_GE = 0xD };
enum { ALU_ADD = 0x01, ALU_OR = 0x09, ALU_SUB = 0x29 }; /* op r/m, r */
enum { EXT_ADD = 0, EXT_AND = 4, EXT_SUB = 5, EXT_CMP = 7 };
enum { SH_SHL = 4, SH_SHR = 5, SH_SAR = 7 };

#define CODE_SIZE 8192
#define TRACE_OFF 4096 /* the trace gets its own page, so it can be written while the VM runs */
static uint8_t *region;
static size_t cur;

static void emit8(int x)
{
    region[cur++] = (uint8_t)x;
}

static void emit32(int32_t x)
{
    memcpy(region + cur, &x, 4);
    cur += 4;
}

struct mem {
    int base, index, scale;
    int32_t disp;
};

static struct mem M(int base, int32_t disp)
{
    return (struct mem){ base, -1, 0, disp };
}

static struct mem MX(int base, int index, int scale, int32_t disp)
{
    return (struct mem){ base, index, scale, disp };
}

static void rex(int w, int reg, int index, int base)
{
    int v = 0x40 | w << 3 | (reg >> 3) << 2 | (index >> 3) << 1 | (base >> 3);
    if (v != 0x40) {
        emit8(v);
    }
}

/* op1 [op2] with a register in the reg field and a memory operand. No memory operand here has
 * rsp or r12 as its base without an index, or rbp or r13 as its base with a zero displacement:
 * those encodings are special and the VM does not need them. */
static void mem_op(int w, int op1, int op2, int reg, struct mem m)
{
    rex(w, reg, m.index < 0 ? 0 : m.index, m.base);
    emit8(op1);
    if (op2 >= 0) {
        emit8(op2);
    }
    int mod = m.disp == 0 ? 0 : (m.disp >= -128 && m.disp <= 127) ? 1 : 2;
    emit8(mod << 6 | (reg & 7) << 3 | (m.index < 0 ? m.base & 7 : 4));
    if (m.index >= 0) {
        emit8(m.scale << 6 | (m.index & 7) << 3 | (m.base & 7));
    }
    if (mod == 1) {
        emit8(m.disp);
    } else if (mod == 2) {
        emit32(m.disp);
    }
}

static void rr_op(int w, int op1, int op2, int reg, int rm)
{
    rex(w, reg, 0, rm);
    emit8(op1);
    if (op2 >= 0) {
        emit8(op2);
    }
    emit8(0xC0 | (reg & 7) << 3 | (rm & 7));
}

static void ld64(int r, struct mem m) { mem_op(1, 0x8B, -1, r, m); }
static void ld32(int r, struct mem m) { mem_op(0, 0x8B, -1, r, m); }
static void st64(struct mem m, int r) { mem_op(1, 0x89, -1, r, m); }
static void sti64(struct mem m, int32_t v) { mem_op(1, 0xC7, -1, 0, m); emit32(v); }
static void mov64(int dst, int src) { rr_op(1, 0x89, -1, src, dst); }
static void mov32(int dst, int src) { rr_op(0, 0x89, -1, src, dst); }
static void lea64(int r, struct mem m) { mem_op(1, 0x8D, -1, r, m); }
static void movzxb(int dst, struct mem m) { mem_op(0, 0x0F, 0xB6, dst, m); }
static void movzxw(int dst, struct mem m) { mem_op(0, 0x0F, 0xB7, dst, m); }
static void movsx16(int dst, int src) { rr_op(0, 0x0F, 0xBF, dst, src); }
static void imul32(int dst, int src) { rr_op(0, 0x0F, 0xAF, dst, src); }
static void alu_rr(int w, int opc, int dst, int src) { rr_op(w, opc, -1, src, dst); }
static void cmp_rm32(int r, struct mem m) { mem_op(0, 0x3B, -1, r, m); }
static void add_rm32(int r, struct mem m) { mem_op(0, 0x03, -1, r, m); }
static void jmp_m(struct mem m) { mem_op(0, 0xFF, -1, 4, m); }
static void call_m(struct mem m) { mem_op(0, 0xFF, -1, 2, m); }

/* movzx r32, r8 with a legacy register: 4 to 7 are ah, ch, dh and bh (no REX prefix) */
static void movzx8(int dst, int src8)
{
    emit8(0x0F);
    emit8(0xB6);
    emit8(0xC0 | dst << 3 | src8);
}

static void movi64(int r, uint64_t v)
{
    rex(1, 0, 0, r);
    emit8(0xB8 | (r & 7));
    memcpy(region + cur, &v, 8);
    cur += 8;
}

/* op r, imm8 (sign-extended) */
static void alu_ri8(int w, int ext, int r, int imm)
{
    rex(w, 0, 0, r);
    emit8(0x83);
    emit8(0xC0 | ext << 3 | (r & 7));
    emit8(imm);
}

static void add_ri32(int w, int r, int32_t imm)
{
    rex(w, 0, 0, r);
    emit8(0x81);
    emit8(0xC0 | (r & 7));
    emit32(imm);
}

static void shift_i(int w, int ext, int r, int n)
{
    rex(w, 0, 0, r);
    emit8(0xC1);
    emit8(0xC0 | ext << 3 | (r & 7));
    emit8(n);
}

/* not (ext 2) and neg (ext 3) */
static void unary(int w, int ext, int r)
{
    rex(w, 0, 0, r);
    emit8(0xF7);
    emit8(0xC0 | ext << 3 | (r & 7));
}

static void test32_i(int r, uint32_t imm)
{
    rex(0, 0, 0, r);
    emit8(0xF7);
    emit8(0xC0 | (r & 7));
    emit32((int32_t)imm);
}

/* sub word [m], imm8: the hot counters are 16 bits */
static void sub_w_mi8(struct mem m, int imm)
{
    emit8(0x66);
    mem_op(0, 0x83, -1, EXT_SUB, m);
    emit8(imm);
}

static void jmp_r(int r)
{
    rex(0, 0, 0, r);
    emit8(0xFF);
    emit8(0xC0 | 4 << 3 | (r & 7));
}

static void push(int r)
{
    rex(0, 0, 0, r);
    emit8(0x50 | (r & 7));
}

static void pop(int r)
{
    rex(0, 0, 0, r);
    emit8(0x58 | (r & 7));
}

static void push_i8(int v)
{
    emit8(0x6A);
    emit8(v);
}

static void ret(void) { emit8(0xC3); }
static void ud2(void) { emit8(0x0F); emit8(0x0B); }

/* Labels. The VM's own labels have fixed numbers, after the opcodes, so the trace can branch to
 * them. Local labels inside a handler are numbered from L_LOCAL. */
enum {
    L_ENTER = BC__MAX, L_RETURN, L_HOTLOOP, L_HOTCALL, L_RECORD, L_REDISPATCH, L_EXIT_HANDLER,
    L_VMETA, L_TRACE_TOP, L_STUB, L_LOCAL, MAXLAB = 64
};
static int nlocal = L_LOCAL;
static long labpos[MAXLAB];

struct fixup {
    size_t at;
    int lab;
};
static struct fixup fixups[256];
static int nfix;

static void bind(int l)
{
    labpos[l] = (long)cur;
}

static void branch_hole(int l)
{
    fixups[nfix++] = (struct fixup){ cur, l };
    emit32(0);
}

static void jmp_l(int l)
{
    emit8(0xE9);
    branch_hole(l);
}

static void jcc_l(int cc, int l)
{
    emit8(0x0F);
    emit8(0x80 | cc);
    branch_hole(l);
}

static void link_code(void)
{
    for (int i = 0; i < nfix; i++) {
        int32_t rel = (int32_t)(labpos[fixups[i].lab] - (long)(fixups[i].at + 4));
        memcpy(region + fixups[i].at, &rel, 4);
    }
    nfix = 0;
}

/* ------------------------------------------------------------------------------------------
 * The interpreter. This is vm_x64.dasc, cut down to the opcodes of the examples. The macros
 * of the original keep their names. Fields A and C of the instruction are decoded by the
 * dispatch itself, in registers, so a handler starts with them ready.
 * ---------------------------------------------------------------------------------------- */

/* Instruction decode and dispatch: load the word, split out A and the opcode, step the pc,
 * leave D (or B and C together) in rax, and jump through the table. Every handler ends with a
 * copy of this, so each has its own indirect jump for the branch predictor. */
static size_t ins_next_start, ins_next_end;
static void ins_next(void)
{
    size_t s = cur;
    ld32(RC, M(PC, 0));             /* mov eax, [PC] */
    movzx8(RA, 4);                  /* movzx RAd, RCH: A is the second byte */
    movzx8(OP, 0);                  /* movzx OP, RCL: the opcode is the first */
    alu_ri8(1, EXT_ADD, PC, 4);     /* add PC, 4 */
    shift_i(0, SH_SHR, RC, 16);     /* shr RCd, 16: D is what is left */
    jmp_m(MX(DISPATCH, OP, 3, 0));  /* jmp [DISPATCH + OP*8] */
    ins_next_start = s;
    ins_next_end = cur;
}

/* Call decode and dispatch: BASE is the new base, RB the function, RD is nargs+1. The first
 * instruction of the callee is a function header, which is dispatched like any other. */
static void ins_callt(void)
{
    ld64(PC, M(RB, (int32_t)offsetof(struct func, pc)));
    ld32(RA, M(PC, 0));
    movzx8(OP, 1);
    movzx8(RA, 5);
    alu_ri8(1, EXT_ADD, PC, 4);
    jmp_m(MX(DISPATCH, OP, 3, 0));
}

static void ins_ABC(void)
{
    movzx8(RB, 4); /* movzx RBd, RCH */
    movzx8(RC, 0); /* movzx RCd, RCL */
}

static void cleartp(int r)
{
    shift_i(1, SH_SHL, r, 17);
    shift_i(1, SH_SHR, r, 17);
}

static void settp(int r, uint32_t tp)
{
    movi64(ITYPE, TAG(tp, 0));
    alu_rr(1, ALU_OR, r, ITYPE);
}

static void checktp(int r, uint32_t tp, int target)
{
    mov64(ITYPE, r);
    shift_i(1, SH_SAR, ITYPE, 47);
    alu_ri8(0, EXT_CMP, ITYPE, (int8_t)tp);
    jcc_l(CC_NE, target);
}

/* extra is 4 when the pc still points at the jump itself, as in ISGE */
static void branchPC(int r, int extra)
{
    lea64(PC, MX(PC, r, 2, extra - BCBIAS_J * 4));
}

/* Decrement the hot counter that the pc hashes to, and call into C when it underflows. */
static void hotcount(int delta, int target)
{
    mov32(RB, PC);
    shift_i(0, SH_SHR, RB, 1);
    alu_ri8(0, EXT_AND, RB, HOTCOUNT_PCMASK);
    sub_w_mi8(MX(DISPATCH, RB, 0, GG(hotcount)), delta);
    jcc_l(CC_B, target);
}

/* The common tail of returns: go back to the caller's frame and constants and continue. */
static void ret_tail(void)
{
    movzxb(RA, M(PC, -3)); /* the A field of the call instruction: where the callee's frame was */
    unary(1, 3, RA);
    lea64(BASE, MX(BASE, RA, 3, -16));
    ld64(KBASE, M(BASE, -16));
    cleartp(KBASE);
    ld64(KBASE, M(KBASE, (int32_t)offsetof(struct func, pc)));
    ld64(KBASE, M(KBASE, PC2PROTO_K));
    ins_next();
}

static void emit_arith(int op)
{
    ins_ABC();
    ld64(RB, MX(BASE, RB, 3, 0));
    ld64(RC, op == BC_ADDVN || op == BC_SUBVN ? MX(KBASE, RC, 3, 0) : MX(BASE, RC, 3, 0));
    checktp(RB, LJ_TISNUM, L_VMETA);
    checktp(RC, LJ_TISNUM, L_VMETA);
    if (op == BC_ADDVN || op == BC_ADDVV) {
        alu_rr(0, ALU_ADD, RB, RC);
    } else if (op == BC_SUBVN) {
        alu_rr(0, ALU_SUB, RB, RC);
    } else {
        imul32(RB, RC);
    }
    jcc_l(CC_O, L_VMETA);
    settp(RB, LJ_TISNUM);
    st64(MX(BASE, RA, 3, 0), RB);
    ins_next();
}


static int newlabel(void)
{
    return nlocal++;
}

static void st32(struct mem m, int r) { mem_op(0, 0x89, -1, r, m); }
static void sti32(struct mem m, int32_t v) { mem_op(0, 0xC7, -1, 0, m); emit32(v); }

static void save_base_and_call(int cbfield)
{
    st64(M(DISPATCH, GG(base)), BASE);
    mov64(RDI, PC);
    call_m(M(DISPATCH, cbfield));
    ld64(BASE, M(DISPATCH, GG(base)));
}

static void emit_ins(int op)
{
    bind(op);
    switch (op) { // GCOVR_EXCL_BR_LINE: every opcode of the loop has a case
    case BC_ISGE: { /* A and D are slots, and a JMP follows: it is taken if A >= D */
        int skip = newlabel();
        ld64(RB, MX(BASE, RA, 3, 0));
        ld64(RC, MX(BASE, RD, 3, 0));
        checktp(RB, LJ_TISNUM, L_VMETA);
        checktp(RC, LJ_TISNUM, L_VMETA);
        alu_rr(0, 0x39, RB, RC); /* cmp ebp, eax */
        jcc_l(CC_L, skip);
        movzxw(RD, M(PC, 2));
        branchPC(RD, 4);
        ins_next();
        bind(skip);
        alu_ri8(1, EXT_ADD, PC, 4);
        ins_next();
        break;
    }
    case BC_KSTR: /* KFUNC is the same code: the constant index is ~D */
        unary(1, 2, RD);
        ld64(RB, MX(KBASE, RD, 3, 0));
        st64(MX(BASE, RA, 3, 0), RB);
        ins_next();
        break;
    case BC_KFUNC:
        break;
    case BC_KSHORT:
        movsx16(RD, RD);
        settp(RD, LJ_TISNUM);
        st64(MX(BASE, RA, 3, 0), RD);
        ins_next();
        break;
    case BC_MOV:
        ld64(RB, MX(BASE, RD, 3, 0));
        st64(MX(BASE, RA, 3, 0), RB);
        ins_next();
        break;
    case BC_ADDVN: case BC_SUBVN: case BC_ADDVV: case BC_MULVV:
        emit_arith(op);
        break;
    case BC_CALL: /* the new base is above the function slot and the pc slot */
        lea64(BASE, MX(BASE, RA, 3, 16));
        st64(M(BASE, -8), PC);
        ld64(RB, M(BASE, -16));
        cleartp(RB);
        ins_callt();
        break;
    case BC_RET0:
        ld64(PC, M(BASE, -8));
        test32_i(PC, FRAME_TYPE);
        jcc_l(CC_NE, L_RETURN);
        ret_tail();
        break;
    case BC_RET1: /* the result goes to the function slot, which is slot A of the caller */
        ld64(RB, MX(BASE, RA, 3, 0));
        st64(M(BASE, -16), RB);
        jmp_l(BC_RET0);
        break;
    case BC_LOOP: /* counts, then falls into ILOOP */
        hotcount(HOTCOUNT_LOOP, L_HOTLOOP);
        break;
    case BC_ILOOP:
        ins_next();
        break;
    case BC_JLOOP: /* D is the trace number: enter its machine code with BASE in place */
        st64(M(DISPATCH, GG(jit_base)), BASE);
        ld64(RC, MX(DISPATCH, RD, 3, GG(trace_mcode)));
        jmp_r(RC);
        break;
    case BC_JMP:
        branchPC(RD, 0);
        ins_next();
        break;
    case BC_FUNCF: /* counts calls, then falls into IFUNCF */
        hotcount(HOTCOUNT_CALL, L_HOTCALL);
        break;
    case BC_IFUNCF:
        ld64(KBASE, M(PC, PC2PROTO_K - 4));
        ins_next();
        break;
    case BC_FUNCC:
        st64(M(DISPATCH, GG(base)), BASE);
        mov64(RDI, BASE);
        ld64(RB, M(BASE, -16));
        cleartp(RB);
        call_m(M(RB, (int32_t)offsetof(struct func, cf)));
        ld64(BASE, M(DISPATCH, GG(base)));
        jmp_l(BC_RET0);
        break;
    }
}

static void emit_vm(void)
{
    /* vm_enter(gg, base): save the callee-saved registers, then call the function in the
     * frame below base. The stack is aligned for the calls into C below. */
    bind(L_ENTER);
    int saved[] = { RBX, RBP, R12, R13, R14, R15 };
    for (int i = 0; i < 6; i++) {
        push(saved[i]);
    }
    alu_ri8(1, EXT_SUB, RSP, 8);
    mov64(DISPATCH, RDI);
    mov64(BASE, RSI);
    ld64(RB, M(BASE, -16));
    cleartp(RB);
    ins_callt();
    /* vm_return: the bottom frame of the VM returned to C */
    bind(L_RETURN);
    alu_ri8(1, EXT_ADD, RSP, 8);
    for (int i = 5; i >= 0; i--) {
        pop(saved[i]);
    }
    ret();
    for (int op = 0; op < BC__MAX; op++) {
        emit_ins(op);
    }
    labpos[BC_KFUNC] = labpos[BC_KSTR];
    /* A counter underflowed. The pc is the biased pc of the next instruction. */
    bind(L_HOTLOOP);
    save_base_and_call(GG(cb_hotloop));
    jmp_l(L_REDISPATCH);
    bind(L_HOTCALL);
    save_base_and_call(GG(cb_hotcall));
    jmp_l(BC_IFUNCF);
    bind(L_RECORD);
    save_base_and_call(GG(cb_record));
    jmp_l(L_REDISPATCH);
    /* Dispatch the instruction before pc again, through the static table: the handlers there
     * do not count or record, so they can run now that the hook has seen the instruction. */
    bind(L_REDISPATCH);
    ld32(RC, M(PC, -4));
    movzx8(RA, 4);
    movzx8(OP, 0);
    shift_i(0, SH_SHR, RC, 16);
    jmp_m(MX(DISPATCH, OP, 3, GG(static_disp)));
    /* vm_exit_handler: a guard of the trace failed. The stub pushed the exit number. Save all
     * the registers, move the trace's base back to where the VM keeps it, and ask C where the
     * interpreter resumes. */
    bind(L_EXIT_HANDLER);
    int exitregs[] = { RAX, RCX, RDX, RBX, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
    for (int i = 0; i < 15; i++) {
        push(exitregs[i]);
    }
    ld64(RAX, M(DISPATCH, GG(jit_base)));
    st64(M(DISPATCH, GG(base)), RAX);
    sti64(M(DISPATCH, GG(jit_base)), 0);
    mov64(RDI, RSP);
    call_m(M(DISPATCH, GG(cb_exit)));
    mov64(PC, RAX);
    add_ri32(1, RSP, 128);
    ld64(BASE, M(DISPATCH, GG(base)));
    ins_next();
    /* a type check failed or an add overflowed: LuaJIT calls the metamethod code here */
    bind(L_VMETA);
    ud2();
}

static const struct {
    const char *name;
    int label;
} vm_labels[] = {
    { "vm_enter", L_ENTER }, { "vm_return", L_RETURN }, { "ISGE", BC_ISGE },
    { "KSTR, KFUNC", BC_KSTR }, { "KSHORT", BC_KSHORT }, { "MOV", BC_MOV },
    { "ADDVN", BC_ADDVN }, { "SUBVN", BC_SUBVN }, { "ADDVV", BC_ADDVV }, { "MULVV", BC_MULVV },
    { "CALL", BC_CALL }, { "RET0", BC_RET0 }, { "RET1", BC_RET1 }, { "LOOP", BC_LOOP },
    { "ILOOP", BC_ILOOP }, { "JLOOP", BC_JLOOP }, { "JMP", BC_JMP }, { "FUNCF", BC_FUNCF },
    { "IFUNCF", BC_IFUNCF }, { "FUNCC", BC_FUNCC }, { "vm_hotloop", L_HOTLOOP },
    { "vm_hotcall", L_HOTCALL }, { "vm_record", L_RECORD }, { "vm_redispatch", L_REDISPATCH },
    { "vm_exit_handler", L_EXIT_HANDLER }, { "vm_meta", L_VMETA },
};

static size_t vm_len;
static size_t trace_len;

static void stage_assemble(void)
{
    region = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(region != MAP_FAILED, "mmap"); // GCOVR_EXCL_LINE: the syscall does not fail
    emit_vm();
    vm_len = cur;
    require(vm_len < TRACE_OFF, "the interpreter has to fit in the first page");
    link_code();
    int rc = mprotect(region, TRACE_OFF, PROT_READ | PROT_EXEC);
    require(rc == 0, "mprotect"); // GCOVR_EXCL_LINE: the syscall does not fail
    P("== stage_assemble: the interpreter as x86-64 machine code, %zu bytes ==\n", vm_len);
    P("  registers: BASE rdx, PC rbx, DISPATCH r14, KBASE r15, RA rcx, RB/OP rbp, RC/RD rax\n");
    P("  every handler ends with ins_next, %zu bytes: decode the word at PC, then jmp [DISPATCH+OP*8]\n",
      ins_next_end - ins_next_start);
    for (size_t i = 0; i < sizeof vm_labels / sizeof vm_labels[0]; i++) {
        P("  +0x%04lx  %s\n", labpos[vm_labels[i].label], vm_labels[i].name);
    }
}

static void set_dispatch(int recording)
{
    for (int op = 0; op < BC__MAX; op++) {
        gg.dispatch[op] = op < BC_FUNCF && recording ? region + labpos[L_RECORD]
                                                     : region + labpos[op];
    }
}

/* ------------------------------------------------------------------------------------------
 * The JIT side, in C. These functions are called from the generated code.
 * ---------------------------------------------------------------------------------------- */

static struct {
    int recording;
    const uint32_t *start;  /* the LOOP instruction the trace starts at */
    int nrec;
    uint32_t rec[8];        /* the recorded instructions */
    const uint32_t *exit_pc; /* where the interpreter resumes when the guard fails */
} jit;

static uint64_t *slot(int i)
{
    return &gg.base[i];
}

static int32_t slot_int(int i)
{
    return (int32_t)(uint32_t)*slot(i);
}

static unsigned hot_slot(const uint32_t *pc)
{
    return (unsigned)(((uintptr_t)pc >> 2) & (HOTCOUNT_SIZE - 1));
}

/* A guard of the trace failed. The exit handler saved the registers and the trace's base. */
static const uint32_t *stage_exit(const struct exitstate *ex)
{
    P("== stage_exit: the guard of exit %llu failed, the registers are saved and C is called ==\n",
      (unsigned long long)ex->exitno);
    P("  slots at the exit: s0=%d s1=%d s2=%d, resume the interpreter at %s %04d\n", slot_int(0),
      slot_int(1), slot_int(2), fn_name(jit.exit_pc), bc_index(jit.exit_pc));
    return jit.exit_pc;
}

/* Recording reached the start LOOP again: make the trace from the recorded instructions. The
 * trace keeps every value in its stack slot and has no type guards. A real trace is made of
 * an IR, optimized, and has its values in registers, see distilled/luajit. */
static void stage_trace(void)
{
    const struct proto *pr = proto_of(jit.start);
    P("== stage_trace: recording stopped at the start LOOP, machine code for the loop body ==\n");
    cur = TRACE_OFF;
    bind(L_TRACE_TOP);
    for (int i = 0; i < jit.nrec; i++) {
        uint32_t w = jit.rec[i];
        switch (bc_op(w)) { // GCOVR_EXCL_BR_LINE: only these opcodes are recorded
        case BC_ADDVV:
            ld32(RAX, M(BASE, (int32_t)bc_b(w) * 8));
            add_rm32(RAX, M(BASE, (int32_t)bc_c(w) * 8));
            st32(M(BASE, (int32_t)bc_a(w) * 8), RAX);
            P("  trace  slot%u = slot%u + slot%u\n", bc_a(w), bc_b(w), bc_c(w));
            break;
        case BC_ADDVN:
            ld32(RAX, M(BASE, (int32_t)bc_b(w) * 8));
            add_ri32(0, RAX, (int32_t)(uint32_t)pr->knum[bc_c(w)]);
            st32(M(BASE, (int32_t)bc_a(w) * 8), RAX);
            P("  trace  slot%u = slot%u + %d\n", bc_a(w), bc_b(w), (int32_t)(uint32_t)pr->knum[bc_c(w)]);
            break;
        case BC_KSHORT:
            sti32(M(BASE, (int32_t)bc_a(w) * 8), (int16_t)bc_d(w));
            P("  trace  slot%u = %d\n", bc_a(w), (int16_t)bc_d(w));
            break;
        case BC_ISGE: /* the loop continues while A < D: the guard exits when A >= D */
            ld32(RAX, M(BASE, (int32_t)bc_a(w) * 8));
            cmp_rm32(RAX, M(BASE, (int32_t)bc_d(w) * 8));
            jcc_l(CC_GE, L_STUB);
            P("  trace  guard slot%u < slot%u, else exit 0\n", bc_a(w), bc_d(w));
            break;
        case BC_JMP: /* the JMP back: the trace jumps to its own top */
            break;
        }
    }
    jmp_l(L_TRACE_TOP);
    P("  trace  jump to the top of the trace\n");
    bind(L_STUB);
    push_i8(0);
    jmp_l(L_EXIT_HANDLER);
    link_code();
    trace_len = cur - TRACE_OFF;
    int rc = mprotect(region + TRACE_OFF, TRACE_OFF, PROT_READ | PROT_EXEC);
    require(rc == 0, "mprotect"); // GCOVR_EXCL_LINE: the syscall does not fail
    gg.trace_mcode[1] = region + TRACE_OFF;
    *(uint32_t *)(uintptr_t)jit.start = BCINS_AD(BC_JLOOP, bc_a(*jit.start), 1);
    set_dispatch(0);
    jit.recording = 0;
    P("  trace 1 is %zu bytes at +0x%04x, exit stub at +0x%04lx\n", trace_len, TRACE_OFF,
      labpos[L_STUB]);
    P("  patched %s %04d from LOOP to JLOOP, trace 1, and the dispatch table is back to normal\n",
      fn_name(jit.start), bc_index(jit.start));
}

/* vm_record calls this before every instruction while recording. */
static void stage_record(const uint32_t *pc)
{
    const uint32_t *ins = pc - 1;
    int op = (int)bc_op(*ins);
    P("== stage_record: vm_record sees %s %04d ==\n", bc_name[op], bc_index(ins));
    if (ins == jit.start) { /* the LOOP itself runs through the static table, unrecorded */
        stage_trace();
        return;
    }
    require(jit.nrec < 8, "a loop of at most eight instructions");
    jit.rec[jit.nrec++] = *ins;
    switch (op) { // GCOVR_EXCL_BR_LINE: only these opcodes are in the loop
    case BC_ISGE: { /* the JMP after it was not taken: the trace assumes it stays so */
        int a = (int)bc_a(*ins), d = (int)bc_d(*ins);
        require(slot_int(a) < slot_int(d), "recording a loop that exits");
        jit.exit_pc = ins + 2 + bc_j(ins[1]);
        P("  not taken, so the exit guard resumes at %s %04d\n", fn_name(jit.exit_pc),
          bc_index(jit.exit_pc));
        break;
    }
    case BC_ADDVV: case BC_ADDVN: case BC_KSHORT: case BC_JMP:
        break;
    // GCOVR_EXCL_START: the examples have no other instruction in a hot loop
    default:
        require(0, "this instruction in a trace");
    // GCOVR_EXCL_STOP
    }
}

/* A LOOP counter underflowed: the loop is hot. Start recording. */
static void jit_hotloop(const uint32_t *pc)
{
    gg.hotcount[hot_slot(pc)] = HOTLOOP * HOTCOUNT_LOOP;
    jit.start = pc - 1;
    jit.nrec = 0;
    jit.recording = 1;
    set_dispatch(1);
    P("== vm_hotloop: LOOP %s %04d is hot, the counter was slot %u ==\n", fn_name(jit.start),
      bc_index(jit.start), hot_slot(pc));
    P("  every dispatch table entry below FUNCF now points to vm_record\n");
}

/* A FUNCF counter underflowed: the function is hot. Function traces are not supported here, so
 * the header becomes IFUNCF, which does not count, as when LuaJIT blacklists a function. */
static void jit_hotcall(const uint32_t *pc)
{
    *(uint32_t *)(uintptr_t)(pc - 1) = BCINS_AD(BC_IFUNCF, bc_a(pc[-1]), 0);
    P("== vm_hotcall: FUNCF %s %04d is hot, counter slot %u, blacklisted as IFUNCF ==\n",
      fn_name(pc - 1), bc_index(pc - 1), hot_slot(pc));
}

static void stage_dispatch(void)
{
    for (int i = 0; i < HOTCOUNT_SIZE; i++) {
        gg.hotcount[i] = HOTLOOP * HOTCOUNT_LOOP - 1;
    }
    set_dispatch(0);
    for (int op = 0; op < BC_FUNCF; op++) {
        gg.static_disp[op] = region + labpos[op == BC_LOOP ? BC_ILOOP : op];
    }
    gg.cb_hotloop = jit_hotloop;
    gg.cb_record = stage_record;
    gg.cb_hotcall = jit_hotcall;
    gg.cb_exit = stage_exit;
    P("== stage_dispatch: dispatch tables and hot counters ==\n");
    P("  the dispatch table has an entry per opcode, the static table has the ones below FUNCF\n");
    P("  the static table maps LOOP to the ILOOP handler, which does not count\n");
    for (int op = 0; op < BC__MAX; op++) {
        P("  %-6s dispatch +0x%04lx", bc_name[op], labpos[op]);
        if (op < BC_FUNCF) {
            P("  static +0x%04lx", labpos[op == BC_LOOP ? BC_ILOOP : op]);
        }
        P("\n");
    }
    P("  hot counters (%d of them, a pc hashes to one) start at %d\n", HOTCOUNT_SIZE,
      gg.hotcount[0]);
    for (int i = 0; i < nprotos; i++) {
        for (int j = 0; j < protos[i].n; j++) {
            int op = (int)bc_op(protos[i].bc[j]);
            if (op == BC_LOOP || op == BC_FUNCF) {
                P("  %s %04d %s uses counter %u\n", i == 0 ? "main" : "fact", j, bc_name[op],
                  hot_slot(&protos[i].bc[j + 1]));
            }
        }
    }
}

static uint64_t vm_stack[STACK_SLOTS];

static void stage_interpret(void)
{
    /* The bottom frame: the main function, and FRAME_C as the return pc */
    vm_stack[0] = TAG(LJ_TFUNC, (uint64_t)(uintptr_t)&main_fn);
    vm_stack[1] = FRAME_C;
    void (*enter)(struct gg *, uint64_t *);
    void *entry = region + labpos[L_ENTER];
    memcpy(&enter, &entry, sizeof enter);
    P("== stage_interpret: call vm_enter at +0x%04lx, the program runs on the generated code ==\n",
      labpos[L_ENTER]);
    fflush(stdout);
    enter(&gg, vm_stack + 2);
    fflush(stdout);
    P("  the bottom frame returned to C, jit_base is clear\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { // GCOVR_EXCL_LINE: checked by check-entry
        return 2;
    }
    size_t k = 0;
    while (k < sizeof examples / sizeof examples[0] && strcmp(examples[k].name, argv[1]) != 0) {
        k++;
    }
    if (k == sizeof examples / sizeof examples[0]) {
        fprintf(stderr, "unknown example: %s\n", argv[1]);
        return 2;
    }
    examples[k].build();
    stage_bytecode();
    stage_assemble();
    stage_dispatch();
    stage_interpret();
    if (argc > 2) {
        FILE *f = fopen(argv[2], "wb");
        require(f != NULL, "open code.bin"); // GCOVR_EXCL_LINE: the path is writable
        fwrite(region, 1, trace_len ? TRACE_OFF + trace_len : vm_len, f);
        fclose(f);
    }
    return 0;
}
