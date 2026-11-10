/*
 * luajit-remake: a cut-down copy-and-patch baseline JIT, after LuaJIT Remake and its VM
 * generator Deegen.
 *
 * LuaJIT Remake does not have a code generator in the usual sense. At build time, Deegen
 * compiles the C++ implementation of every bytecode with LLVM and reads the resulting object
 * file. The machine code of each bytecode becomes a "stencil": a template of bytes in which
 * every value that is only known when the JIT runs (a stack slot, a constant, a jump target)
 * is a hole, recorded as a relocation. At run time the baseline JIT only copies the stencil of
 * each bytecode into the code buffer and writes the values into its holes. There is no
 * instruction selection, no register allocation and no assembler.
 *
 * This entry keeps that shape. The stencils are machine-code byte arrays written by hand
 * (standing in for the LLVM output), with a table of holes for each one. The programs are
 * bytecode written by hand, in a small register bytecode of this entry's own.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the form after each stage on stderr. code.bin
 *   receives the JIT'ed code with the host addresses (which differ on every run) zeroed.
 *
 * Stages:
 *   stage_bytecode  the bytecode of each function of the example
 *   stage_stencils  the stencil library: bytes and holes of every stencil in use
 *   stage_layout    where each bytecode's fast path and the slow path land in the buffer
 *   stage_patch     copy each stencil into the buffer and fill its holes
 *   stage_export    write code.bin, then make the buffer executable and run the main chunk
 */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

#define MAX_FUNCS 2
#define MAX_CODE 16
#define MAX_CONSTS 3
#define MAX_BUF 1024
#define STACK_SLOTS 64

/* ------------------------------------------------------------------------------------------
 * Values. A slot is 16 bytes: a type tag and a payload. The JIT'ed code tests the tag.
 * ---------------------------------------------------------------------------------------- */

enum { T_NIL, T_INT, T_STR, T_FUNC };

struct slot {
    int64_t tag;
    int64_t val;
};

/* The first field is the entry point, which the code of a call loads and calls. A builtin has
 * a C function there, and a Lua function has its JIT'ed code once stage_patch has run. */
struct function {
    void (*entry)(struct slot *base);
};

static void rt_print(struct slot *base)
{
    if (base[0].tag == T_STR) {
        printf("%s\n", (const char *)(intptr_t)base[0].val);
    } else {
        printf("%lld\n", (long long)base[0].val);
    }
}

/* The slow path of every stencil: a value had the wrong type. The fast paths only handle
 * integers and functions, and the examples never leave them. */
// GCOVR_EXCL_START: no example reaches a slow path
static void rt_type_error(void)
{
    fprintf(stderr, "type error\n");
    exit(1);
}
// GCOVR_EXCL_STOP

/* ------------------------------------------------------------------------------------------
 * Bytecode. Registers are numbered slots of the frame. A call has the function in slot A and
 * the arguments after it, and the callee's frame starts at slot A + 1. The callee stores its
 * result in the slot before its own frame, which is slot A of the caller.
 * ---------------------------------------------------------------------------------------- */

enum op { OP_LOADI, OP_LOADK, OP_MOVE, OP_ADD, OP_SUB, OP_MUL, OP_JNLT, OP_JMP, OP_CALL,
          OP_RET1, OP_RET0 };

static const char *const op_name[] = { "LOADI", "LOADK", "MOVE", "ADD", "SUB", "MUL", "JNLT",
                                       "JMP", "CALL", "RET1", "RET0" };

struct insn {
    enum op op;
    int a, b, c; /* slots, or b is the constant number of LOADK */
    int imm;     /* LOADI: the integer. JNLT, JMP: the index of the target instruction */
};

struct proto {
    const char *name;
    int n;
    struct insn code[MAX_CODE];
    struct function fn;
};

static struct function print_fn = { rt_print };

static const char hello[] = "Hello, world!";

static struct slot consts[MAX_CONSTS];

static struct proto protos[MAX_FUNCS];
static int nprotos;

static void set_const(int k, int64_t tag, const void *p)
{
    consts[k].tag = tag;
    consts[k].val = (int64_t)(intptr_t)p;
}

static void add_proto(const char *name, int n, const struct insn *code)
{
    struct proto *p = &protos[nprotos++];
    p->name = name;
    p->n = n;
    memcpy(p->code, code, (size_t)n * sizeof *code);
}

#define I(op, a, b, c, imm) { op, a, b, c, imm }

static void example_hello(void)
{
    set_const(0, T_FUNC, &print_fn);
    set_const(1, T_STR, hello);
    const struct insn code[] = {
        I(OP_LOADK, 0, 0, 0, 0), I(OP_LOADK, 1, 1, 0, 0), I(OP_CALL, 0, 0, 0, 0),
        I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 4, code);
}

static void example_arithmetic(void)
{
    set_const(0, T_FUNC, &print_fn);
    const struct insn code[] = {
        I(OP_LOADK, 0, 0, 0, 0), I(OP_LOADI, 1, 0, 0, 2), I(OP_LOADI, 2, 0, 0, 3),
        I(OP_ADD, 1, 1, 2, 0),   I(OP_LOADI, 2, 0, 0, 4), I(OP_MUL, 1, 1, 2, 0),
        I(OP_LOADI, 2, 0, 0, 6), I(OP_SUB, 1, 1, 2, 0),   I(OP_CALL, 0, 0, 0, 0),
        I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 10, code);
}

static void example_branch(void)
{
    set_const(0, T_FUNC, &print_fn);
    const struct insn code[] = {
        I(OP_LOADI, 0, 0, 0, 3),  I(OP_LOADI, 1, 0, 0, 5), I(OP_JNLT, 0, 0, 1, 8),
        I(OP_LOADK, 2, 0, 0, 0),  I(OP_MOVE, 3, 1, 0, 0),  I(OP_CALL, 2, 0, 0, 0),
        I(OP_JMP, 0, 0, 0, 11),   I(OP_RET0, 0, 0, 0, 0),  I(OP_LOADK, 2, 0, 0, 0),
        I(OP_MOVE, 3, 0, 0, 0),   I(OP_CALL, 2, 0, 0, 0),  I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 12, code);
}

static void example_loop(void)
{
    set_const(0, T_FUNC, &print_fn);
    const struct insn code[] = {
        I(OP_LOADI, 0, 0, 0, 0), I(OP_LOADI, 1, 0, 0, 1), I(OP_LOADI, 2, 0, 0, 11),
        I(OP_JNLT, 0, 1, 2, 9),  I(OP_ADD, 0, 0, 1, 0),   I(OP_LOADI, 3, 0, 0, 1),
        I(OP_ADD, 1, 1, 3, 0),   I(OP_JMP, 0, 0, 0, 3),   I(OP_RET0, 0, 0, 0, 0),
        I(OP_LOADK, 2, 0, 0, 0), I(OP_MOVE, 3, 0, 0, 0),  I(OP_CALL, 2, 0, 0, 0),
        I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 13, code);
}

static void example_call(void)
{
    set_const(0, T_FUNC, &print_fn);
    set_const(1, T_FUNC, &protos[1].fn);
    const struct insn code[] = {
        I(OP_LOADK, 0, 0, 0, 0), I(OP_LOADK, 1, 1, 0, 0), I(OP_LOADI, 2, 0, 0, 5),
        I(OP_CALL, 1, 0, 0, 0),  I(OP_CALL, 0, 0, 0, 0),  I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 6, code);
    const struct insn fact[] = {
        I(OP_LOADI, 1, 0, 0, 2), I(OP_JNLT, 0, 0, 1, 5), I(OP_LOADI, 1, 0, 0, 1),
        I(OP_RET1, 1, 0, 0, 0),  I(OP_RET0, 0, 0, 0, 0), I(OP_LOADK, 1, 1, 0, 0),
        I(OP_LOADI, 2, 0, 0, 1), I(OP_SUB, 2, 0, 2, 0),  I(OP_CALL, 1, 0, 0, 0),
        I(OP_MUL, 1, 0, 1, 0),   I(OP_RET1, 1, 0, 0, 0),
    };
    add_proto("fact", 11, fact);
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

static void stage_bytecode(void)
{
    P("== stage_bytecode ==\n");
    for (int f = 0; f < nprotos; f++) {
        const struct proto *p = &protos[f];
        P("function <%s> (%d instructions)\n", p->name, p->n);
        for (int i = 0; i < p->n; i++) {
            const struct insn *in = &p->code[i];
            P("  %2d  %-6s %d %d %d imm=%d\n", i, op_name[in->op], in->a, in->b, in->c, in->imm);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Stencils. Each is a byte array of x86-64 machine code with zeroed holes and a table of the
 * holes. Every operand is addressed as [rbx + disp32], where rbx is the frame base and the
 * displacement is a hole, so one stencil serves every choice of slots. The scratch registers
 * are rax, rcx and rdx.
 *
 * A hole says where it is, how wide it is, and which value the JIT writes into it. The
 * relative kinds are jump offsets, written as destination - (end of the hole).
 *
 * Every stencil that continues to the next bytecode ends with a `jmp rel32` to the next
 * bytecode's code (flagged by `next`), because a stencil cannot know what follows it. Its
 * hole is not in the table: the JIT always places the next bytecode right after, so the jump
 * is cut off before the copy and the hole never exists.
 * ---------------------------------------------------------------------------------------- */

enum src {
    S_A_TAG,      /* disp32 of the tag of slot A */
    S_A_VAL,      /* disp32 of the payload of slot A */
    S_B_TAG,
    S_B_VAL,
    S_C_TAG,
    S_C_VAL,
    S_ARG_BASE,   /* disp32 of slot A + 1 */
    S_IMM,        /* imm32 of the instruction */
    S_CONST_ADDR, /* imm64: the address of constant number B */
    S_ERR_ADDR,   /* imm64: the address of rt_type_error */
    S_TARGET,     /* rel32 to the bytecode named by imm */
    S_SLOW        /* rel32 to the slow path of the function */
};

static const char *const src_name[] = { "A.tag", "A.val",     "B.tag",   "B.val", "C.tag",
                                        "C.val", "argbase",   "imm",     "&const[B]",
                                        "&rt_type_error", "target", "slow" };
static const int src_width[] = { 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 4, 4 };

struct hole {
    int off;
    enum src src;
};

struct stencil {
    const char *name;
    int len;
    int nholes;
    struct hole holes[10];
    uint8_t bytes[80];
    int next; /* 1 if the last 5 bytes are a `jmp rel32` to the next bytecode */
};

#define Z 0, 0, 0, 0
#define Z8 0, 0, 0, 0, 0, 0, 0, 0

/* The type tests that guard the integer fast paths of ADD, SUB, MUL and JNLT:
 *   cmp qword [rbx+B.tag], T_INT ; jne slow ; cmp qword [rbx+C.tag], T_INT ; jne slow */
#define CHECK_BC                                                                         \
    0x48, 0x83, 0xBB, Z, 0x01, 0x0F, 0x85, Z, 0x48, 0x83, 0xBB, Z, 0x01, 0x0F, 0x85, Z
#define CHECK_BC_HOLES { 3, S_B_TAG }, { 10, S_SLOW }, { 17, S_C_TAG }, { 24, S_SLOW }

/* An arithmetic stencil: check both tags, load B and C, apply the op, store the integer.
 * OPLEN is the length of the op bytes, which differs between add and imul. */
#define ARITH(NAME, OPLEN, ...)                                                          \
    {                                                                                    \
        NAME, 65 + (OPLEN),                                                              \
        8,                                                                               \
        { CHECK_BC_HOLES, { 31, S_B_VAL }, { 38, S_C_VAL }, { 45 + (OPLEN), S_A_VAL }, \
          { 52 + (OPLEN), S_A_TAG } },                         \
        { CHECK_BC, 0x48, 0x8B, 0x83, Z, 0x48, 0x8B, 0x8B, Z, __VA_ARGS__,               \
          0x48, 0x89, 0x83, Z, 0x48, 0xC7, 0x83, Z, 0x01, 0, 0, 0, 0xE9, Z },            \
        .next = 1                                                                        \
    }

static const struct stencil stencils[] = {
    /* push rbx ; mov rbx, rdi: the frame base lives in rbx while the function runs */
    { "PROLOGUE", 4, 0, { { 0 } }, { 0x53, 0x48, 0x89, 0xFB }, .next = 0 },
    /* mov qword [rbx+A.tag], T_INT ; mov qword [rbx+A.val], imm ; jmp next */
    { "LOADI", 27, 3,
      { { 3, S_A_TAG }, { 14, S_A_VAL }, { 18, S_IMM } },
      { 0x48, 0xC7, 0x83, Z, 0x01, 0, 0, 0, 0x48, 0xC7, 0x83, Z, Z, 0xE9, Z }, .next = 1 },
    /* movabs rax, &const ; copy its 16 bytes to slot A ; jmp next */
    { "LOADK", 36, 3,
      { { 2, S_CONST_ADDR }, { 20, S_A_TAG }, { 27, S_A_VAL } },
      { 0x48, 0xB8, Z8, 0x48, 0x8B, 0x08, 0x48, 0x8B, 0x50, 0x08, 0x48, 0x89, 0x8B, Z,
        0x48, 0x89, 0x93, Z, 0xE9, Z }, .next = 1 },
    /* copy slot B to slot A ; jmp next */
    { "MOVE", 33, 4,
      { { 3, S_B_TAG }, { 10, S_B_VAL }, { 17, S_A_TAG }, { 24, S_A_VAL } },
      { 0x48, 0x8B, 0x83, Z, 0x48, 0x8B, 0x8B, Z, 0x48, 0x89, 0x83, Z, 0x48, 0x89, 0x8B, Z,
        0xE9, Z }, .next = 1 },
    /* add rax, rcx */
    ARITH("ADD", 3, 0x48, 0x01, 0xC8),
    /* sub rax, rcx */
    ARITH("SUB", 3, 0x48, 0x29, 0xC8),
    /* imul rax, rcx */
    ARITH("MUL", 4, 0x48, 0x0F, 0xAF, 0xC1),
    /* check tags ; mov rax,[B.val] ; cmp rax,[C.val] ; jge target ; jmp next */
    { "JNLT", 53, 7,
      { CHECK_BC_HOLES, { 31, S_B_VAL }, { 38, S_C_VAL }, { 44, S_TARGET } },
      { CHECK_BC, 0x48, 0x8B, 0x83, Z, 0x48, 0x3B, 0x83, Z, 0x0F, 0x8D, Z, 0xE9, Z }, .next = 1 },
    /* jmp target: the stencil does not continue to the next bytecode */
    { "JMP", 5, 1, { { 1, S_TARGET } }, { 0xE9, Z }, .next = 0 },
    /* cmp qword [rbx+A.tag], T_FUNC ; jne slow ; mov rax,[A.val] ; lea rdi,[rbx+argbase] ;
     * call [rax] ; jmp next. The callee's entry point is the first field of the function. */
    { "CALL", 35, 4,
      { { 3, S_A_TAG }, { 10, S_SLOW }, { 17, S_A_VAL }, { 24, S_ARG_BASE } },
      { 0x48, 0x83, 0xBB, Z, 0x03, 0x0F, 0x85, Z, 0x48, 0x8B, 0x83, Z, 0x48, 0x8D, 0xBB, Z,
        0xFF, 0x10, 0xE9, Z }, .next = 1 },
    /* store slot A into the slot before the frame ; pop rbx ; ret */
    { "RET1", 24, 2,
      { { 3, S_A_TAG }, { 10, S_A_VAL } },
      { 0x48, 0x8B, 0x83, Z, 0x48, 0x8B, 0x8B, Z, 0x48, 0x89, 0x43, 0xF0, 0x48, 0x89, 0x4B,
        0xF8, 0x5B, 0xC3 }, .next = 0 },
    /* pop rbx ; ret */
    { "RET0", 2, 0, { { 0 } }, { 0x5B, 0xC3 }, .next = 0 },
    /* movabs rax, &rt_type_error ; call rax: shared by every guard of the function */
    { "SLOW", 12, 1, { { 2, S_ERR_ADDR } }, { 0x48, 0xB8, Z8, 0xFF, 0xD0 }, .next = 0 },
};

enum { ST_PROLOGUE, ST_LOADI, ST_LOADK, ST_MOVE, ST_ADD, ST_SUB, ST_MUL, ST_JNLT, ST_JMP,
       ST_CALL, ST_RET1, ST_RET0, ST_SLOW };

/* The bytecode op picks the stencil. */
static const int stencil_of_op[] = { ST_LOADI, ST_LOADK, ST_MOVE, ST_ADD, ST_SUB, ST_MUL,
                                     ST_JNLT, ST_JMP, ST_CALL, ST_RET1, ST_RET0 };

/* A stencil that ends in the jump to the next bytecode. When the next bytecode's code is
 * placed right after, that 5-byte jump is not needed, and the JIT leaves it out. */
static int has_next(const struct stencil *s)
{
    return s->next;
}

static int fast_len(const struct stencil *s)
{
    return has_next(s) ? s->len - 5 : s->len;
}

static int used[sizeof stencils / sizeof *stencils];

static void stage_stencils(void)
{
    P("== stage_stencils ==\n");
    used[ST_PROLOGUE] = 1;
    used[ST_SLOW] = 1;
    for (int f = 0; f < nprotos; f++) {
        for (int i = 0; i < protos[f].n; i++) {
            used[stencil_of_op[protos[f].code[i].op]] = 1;
        }
    }
    for (size_t s = 0; s < sizeof stencils / sizeof *stencils; s++) {
        if (!used[s]) {
            continue;
        }
        const struct stencil *st = &stencils[s];
        P("stencil %s (%d bytes, %d holes)\n  bytes:", st->name, st->len, st->nholes);
        for (int i = 0; i < st->len; i++) {
            P(" %02x", st->bytes[i]);
        }
        P("\n");
        for (int h = 0; h < st->nholes; h++) {
            P("  hole +%-2d %d bytes %s\n", st->holes[h].off, src_width[st->holes[h].src],
              src_name[st->holes[h].src]);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * The JIT. Each function becomes: PROLOGUE, the fast path of every bytecode in order, and then
 * the slow path. The offsets are fixed before any hole is written, because a jump needs the
 * offset of a bytecode that comes later.
 * ---------------------------------------------------------------------------------------- */

static uint8_t buf[MAX_BUF];
static int func_off[MAX_FUNCS];
static int insn_off[MAX_FUNCS][MAX_CODE];
static int slow_off[MAX_FUNCS];
static int buf_len;

static void stage_layout(void)
{
    P("== stage_layout ==\n");
    int off = 0;
    for (int f = 0; f < nprotos; f++) {
        const struct proto *p = &protos[f];
        off = (off + 15) & ~15;
        func_off[f] = off;
        P("function <%s> at +0x%03x\n", p->name, off);
        off += stencils[ST_PROLOGUE].len;
        for (int i = 0; i < p->n; i++) {
            const struct stencil *st = &stencils[stencil_of_op[p->code[i].op]];
            insn_off[f][i] = off;
            P("  %2d  %-6s +0x%03x  %2d bytes", i, op_name[p->code[i].op], off, fast_len(st));
            P(has_next(st) ? "  (jump to next dropped)\n" : "\n");
            off += fast_len(st);
        }
        slow_off[f] = off;
        P("      SLOW   +0x%03x  %2d bytes\n", off, stencils[ST_SLOW].len);
        off += stencils[ST_SLOW].len;
    }
    buf_len = off;
    P("total %d bytes\n", buf_len);
}

/* The host addresses written into the buffer. They differ on every run, so code.bin has
 * them zeroed, and the dump shows them by name. */
static int abs_off[MAX_BUF / 8];
static int nabs;

static void put(int off, uint64_t v, int width)
{
    memcpy(buf + off, &v, (size_t)width);
}

static void patch(int f, const struct insn *in, int at, const struct stencil *st)
{
    memcpy(buf + at, st->bytes, (size_t)fast_len(st));
    /* Every instruction fills in the whole table, and only the holes of its stencil use it, so
     * the entries that mean nothing to this instruction are in range but unused. */
    /* The value that every kind of hole takes for this instruction. The jumps are relative to
     * the end of their hole, so they are filled in below, once the hole's place is known. */
    uint64_t value[] = {
        [S_A_TAG] = 16 * (uint64_t)in->a,
        [S_A_VAL] = 16 * (uint64_t)in->a + 8,
        [S_B_TAG] = 16 * (uint64_t)in->b,
        [S_B_VAL] = 16 * (uint64_t)in->b + 8,
        [S_C_TAG] = 16 * (uint64_t)in->c,
        [S_C_VAL] = 16 * (uint64_t)in->c + 8,
        [S_ARG_BASE] = 16 * (uint64_t)(in->a + 1),
        [S_IMM] = (uint64_t)(int64_t)in->imm,
        [S_CONST_ADDR] = (uint64_t)(uintptr_t)&consts[in->b],
        [S_ERR_ADDR] = (uint64_t)(uintptr_t)&rt_type_error,
        [S_TARGET] = (uint64_t)insn_off[f][in->imm],
        [S_SLOW] = (uint64_t)slow_off[f],
    };
    for (int h = 0; h < st->nholes; h++) {
        enum src s = st->holes[h].src;
        int where = at + st->holes[h].off;
        uint64_t v = value[s];
        if (s >= S_TARGET) {
            v -= (uint64_t)(where + 4);
        }
        put(where, v, src_width[s]);
        if (src_width[s] == 8) {
            abs_off[nabs++] = where;
        }
        P("    +0x%03x  %-14s ", where, src_name[s]);
        if (src_width[s] == 8) {
            P("<host address>\n");
        } else if (s == S_TARGET || s == S_SLOW) {
            P("-> +0x%03x\n", where + 4 + (int32_t)v);
        } else {
            P("%d\n", (int32_t)v);
        }
    }
}

static void stage_patch(void)
{
    P("== stage_patch ==\n");
    for (int f = 0; f < nprotos; f++) {
        const struct proto *p = &protos[f];
        P("function <%s>\n", p->name);
        memcpy(buf + func_off[f], stencils[ST_PROLOGUE].bytes, (size_t)stencils[ST_PROLOGUE].len);
        for (int i = 0; i < p->n; i++) {
            const struct insn *in = &p->code[i];
            P("  %2d  %s +0x%03x\n", i, op_name[in->op], insn_off[f][i]);
            patch(f, in, insn_off[f][i], &stencils[stencil_of_op[in->op]]);
        }
        P("  SLOW +0x%03x\n", slow_off[f]);
        patch(f, &p->code[0], slow_off[f], &stencils[ST_SLOW]);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_export
 * ---------------------------------------------------------------------------------------- */

static void stage_export(const char *path)
{
    P("== stage_export ==\n");
    if (path != NULL) {
        uint8_t copy[MAX_BUF];
        memcpy(copy, buf, (size_t)buf_len);
        for (int i = 0; i < nabs; i++) {
            memset(copy + abs_off[i], 0, 8);
        }
        FILE *fp = fopen(path, "wb");
        // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
        if (fp == NULL || fwrite(copy, 1, (size_t)buf_len, fp) != (size_t)buf_len ||
            fclose(fp) != 0) {
            perror(path);
            exit(1);
        }
        // GCOVR_EXCL_STOP
    }
    uint8_t *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (code == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    memcpy(code, buf, (size_t)buf_len);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (mprotect(code, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    /* The buffer was patched in place at its own offsets, and the absolute addresses it holds
     * are those of the constants, not of the code, so moving it to the executable page needs no
     * second pass. The entry points of the Lua functions are the only addresses of the code. */
    /* ISO C forbids casting an object pointer to a function pointer, so copy the bits. */
    void *entry;
    for (int f = 0; f < nprotos; f++) {
        entry = code + func_off[f];
        memcpy(&protos[f].fn.entry, &entry, sizeof entry);
    }
    static struct slot stack[STACK_SLOTS];
    protos[0].fn.entry(stack + 1);
}

int main(int argc, char **argv)
{
    int e = -1;
    for (int i = 0; argc > 1 && i < (int)(sizeof examples / sizeof *examples); i++) {
        if (strcmp(argv[1], examples[i].name) == 0) {
            e = i;
        }
    }
    if (e < 0) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    examples[e].build();
    stage_bytecode();
    stage_stencils();
    stage_layout();
    stage_patch();
    stage_export(argc > 2 ? argv[2] : NULL);
    return 0;
}
