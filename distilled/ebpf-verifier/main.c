/*
 * ebpf-verifier: a cut-down version of the abstract interpreter inside the Linux eBPF verifier.
 *
 * The kernel loads a program from a user and must prove, before running it, that it cannot read
 * an uninitialized register, step outside the memory it was given, or loop forever. It proves
 * this by running the program on abstract values: every scalar register is a pair of facts, a
 * tnum (which bits are known, which are not) and a circular number (an arc of the 64-bit circle
 * that holds the value, which serves as the unsigned and the signed range at once). The two
 * facts refine each other. A conditional jump either has a known outcome or splits the state in
 * two, each side refined by the comparison. The search is depth first, and the verifier saves a
 * state at every jump target. A later path that arrives with registers inside the ranges of a
 * saved state is pruned, but only the registers that the saved state's own exploration
 * depended on ("precise" ones, found by walking the path backwards) must fit. The others match
 * anything.
 *
 * The program is a list of eBPF instructions: no ELF loader, no maps, no helper calls, no
 * subprograms. A program gets r1, a pointer to a buffer of a given size, and r10, a pointer to
 * a 64-byte stack. The only instructions are the 64-bit ALU ones (mov, add, sub, mul, and, or,
 * xor, lsh, rsh), loads and stores, eleven kinds of conditional jump, goto and exit.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints `accept` or `reject` on stdout and the intermediate form after each stage on stderr.
 *   The program is not run and generates no machine code, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source   the program as the loader receives it
 *   stage_cfg      the prune points: jump targets and the instruction after a conditional jump
 *   stage_verify   the depth first search over abstract states: ALU on tnum and cnum, branch
 *                  outcome and refinement, memory checks, saved states and pruning, precision
 *                  marks propagated backwards
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t u64;
typedef int64_t s64;
__extension__ typedef unsigned __int128 u128;
__extension__ typedef __int128 s128;

#define MAX_INSNS 48
#define NREGS 11
#define R_FP 10
#define STACK_SLOTS 8
#define MAX_VAR_OFF (1LL << 29)
#define COMPLEXITY_LIMIT 256
#define MAX_STATES (2 * COMPLEXITY_LIMIT + 8)

/*
 * Branch free selection, minimum and maximum. The verifier takes many decisions on values, and a
 * decision that is written as data cannot be left half covered by the examples.
 */
static u64 sel_u64(bool c, u64 a, u64 b)
{
    return b ^ ((a ^ b) & (0 - (u64)c));
}

static u64 min_u64(u64 a, u64 b)
{
    return sel_u64(a < b, a, b);
}

static u64 max_u64(u64 a, u64 b)
{
    return sel_u64(a < b, b, a);
}

static s128 min_s128(s128 a, s128 b)
{
    return b ^ ((a ^ b) & -(s128)(a < b));
}

static s128 max_s128(s128 a, s128 b)
{
    return a ^ ((a ^ b) & -(s128)(a < b));
}

/* ---------------------------------------------------------------------------------------- */
/* tnum: tracked numbers (kernel/bpf/tnum.c). A bit is known 0, known 1, or unknown.        */
/* ---------------------------------------------------------------------------------------- */

/* A bit set in `mask` is unknown. `value` holds the known bits and is 0 where `mask` is 1. */
typedef struct {
    u64 value, mask;
} Tnum;

#define TNUM_UNKNOWN ((Tnum){0, UINT64_MAX})

static Tnum sel_tnum(bool c, Tnum a, Tnum b)
{
    return (Tnum){sel_u64(c, a.value, b.value), sel_u64(c, a.mask, b.mask)};
}

static Tnum tnum_const(u64 v)
{
    return (Tnum){v, 0};
}

static bool tnum_is_const(Tnum a)
{
    return a.mask == 0;
}

/* The number of bits it takes to write x: 0 for 0, 64 for a number with the top bit set. */
static int fls64(u64 x)
{
    return 64 - __builtin_clzll(x | 1) - (int)(x == 0);
}

/* The tightest tnum that holds every number from min to max: the common prefix is known. */
static Tnum tnum_range(u64 min, u64 max)
{
    int bits = fls64(min ^ max);
    u64 delta = sel_u64(bits > 63, UINT64_MAX, (1ULL << (bits & 63)) - 1);
    return (Tnum){min & ~delta, delta};
}

static Tnum tnum_lshift(Tnum a, int shift)
{
    return (Tnum){a.value << shift, a.mask << shift};
}

static Tnum tnum_rshift(Tnum a, int shift)
{
    return (Tnum){a.value >> shift, a.mask >> shift};
}

/*
 * Addition: add the unknown bits as if they were all 1 and as if they were all 0. A carry that
 * differs between the two sums is an unknown carry, so the bit it lands on becomes unknown.
 */
static Tnum tnum_add(Tnum a, Tnum b)
{
    u64 sm = a.mask + b.mask;
    u64 sv = a.value + b.value;
    u64 sigma = sm + sv;
    u64 chi = sigma ^ sv;
    u64 mu = chi | a.mask | b.mask;
    return (Tnum){sv & ~mu, mu};
}

static Tnum tnum_sub(Tnum a, Tnum b)
{
    u64 dv = a.value - b.value;
    u64 alpha = dv + a.mask;
    u64 beta = dv - b.mask;
    u64 chi = alpha ^ beta;
    u64 mu = chi | a.mask | b.mask;
    return (Tnum){dv & ~mu, mu};
}

static Tnum tnum_and(Tnum a, Tnum b)
{
    u64 alpha = a.value | a.mask;
    u64 beta = b.value | b.mask;
    u64 v = a.value & b.value;
    return (Tnum){v, alpha & beta & ~v};
}

static Tnum tnum_or(Tnum a, Tnum b)
{
    u64 v = a.value | b.value;
    u64 mu = a.mask | b.mask;
    return (Tnum){v, mu & ~v};
}

static Tnum tnum_xor(Tnum a, Tnum b)
{
    u64 v = a.value ^ b.value;
    u64 mu = a.mask | b.mask;
    return (Tnum){v & ~mu, mu};
}

/* Everything that is in a or in b, plus whatever the two disagree on. */
static Tnum tnum_union(Tnum a, Tnum b)
{
    u64 v = a.value & b.value;
    u64 mu = (a.value ^ b.value) | a.mask | b.mask;
    return (Tnum){v & ~mu, mu};
}

/*
 * Long multiplication, one bit of a at a time: a known 0 adds nothing, a known 1 adds b, an
 * unknown bit takes the union of both cases.
 */
static Tnum tnum_mul(Tnum a, Tnum b)
{
    Tnum acc = tnum_const(0);
    while ((a.value | a.mask) != 0) {
        if (a.value & 1) {
            acc = tnum_add(acc, b);
        } else if (a.mask & 1) {
            acc = tnum_union(acc, tnum_add(acc, b));
        }
        a = tnum_rshift(a, 1);
        b = tnum_lshift(b, 1);
    }
    return acc;
}

/* True when some number is in both. */
static bool tnum_overlap(Tnum a, Tnum b)
{
    u64 mu = ~a.mask & ~b.mask;
    return (a.value & mu) == (b.value & mu);
}

/* Both facts are true: a bit known in either is known. */
static Tnum tnum_intersect(Tnum a, Tnum b)
{
    u64 v = a.value | b.value;
    u64 mu = a.mask & b.mask;
    return (Tnum){v & ~mu, mu};
}

/* True when every number of b is in a. */
static bool tnum_in(Tnum a, Tnum b)
{
    b.value &= ~a.mask;
    return (int)((b.mask & ~a.mask) == 0) & (int)(a.value == b.value);
}

/* ---------------------------------------------------------------------------------------- */
/* cnum: circular numbers (kernel/bpf/cnum.c). An arc of the circle of 2^64 values.         */
/* ---------------------------------------------------------------------------------------- */

/*
 * The arc starts at `base` and runs clockwise over `size` further values. It covers the values
 * base, base + 1, ..., base + size, modulo 2^64. Unsigned and signed numbers sit on the same
 * circle: 0 at the top, UINT64_MAX one step before it, INT64_MAX and INT64_MIN at the bottom.
 * An arc that covers the whole circle always has base 0.
 */
typedef struct {
    u64 base, size;
} Cnum;

#define CNUM_UNBOUNDED ((Cnum){0, UINT64_MAX})
#define CNUM_EMPTY ((Cnum){UINT64_MAX, UINT64_MAX})

static Cnum sel_cnum(bool c, Cnum a, Cnum b)
{
    return (Cnum){sel_u64(c, a.base, b.base), sel_u64(c, a.size, b.size)};
}

static bool cnum_is_empty(Cnum c)
{
    return (int)(c.base == UINT64_MAX) & (int)(c.size == UINT64_MAX);
}

static Cnum cnum_normalize(Cnum c)
{
    return (Cnum){sel_u64(c.size == UINT64_MAX, 0, c.base), c.size};
}

static Cnum cnum_from_urange(u64 min, u64 max)
{
    return (Cnum){min, max - min};
}

static Cnum cnum_from_srange(s64 min, s64 max)
{
    return cnum_normalize((Cnum){(u64)min, (u64)max - (u64)min});
}

/* The arc crosses the point between UINT64_MAX and 0, so it is two unsigned ranges. */
static bool cnum_urange_overflow(Cnum c)
{
    return c.size > UINT64_MAX - c.base;
}

static bool cnum_contains(Cnum c, u64 v)
{
    return v - c.base <= c.size;
}

/* The arc crosses the point between INT64_MAX and INT64_MIN, so it is two signed ranges. */
static bool cnum_srange_overflow(Cnum c)
{
    return (int)cnum_contains(c, (u64)INT64_MAX) & (int)cnum_contains(c, (u64)INT64_MIN);
}

/* The unsigned bounds of an arc that crosses the top are the whole range. */
static u64 cnum_umin(Cnum c)
{
    return sel_u64(cnum_urange_overflow(c), 0, c.base);
}

static u64 cnum_umax(Cnum c)
{
    return sel_u64(cnum_urange_overflow(c), UINT64_MAX, c.base + c.size);
}

static s64 cnum_smin(Cnum c)
{
    return (s64)sel_u64(cnum_srange_overflow(c), (u64)INT64_MIN, c.base);
}

static s64 cnum_smax(Cnum c)
{
    return (s64)sel_u64(cnum_srange_overflow(c), (u64)INT64_MAX, c.base + c.size);
}

/*
 * The values that are in both arcs. Two arcs can meet in two pieces, which one arc cannot
 * hold, so then the result is the smaller of the two (a superset, which is safe).
 */
static Cnum cnum_intersect(Cnum a0, Cnum b0)
{
    bool swap = a0.base > b0.base;
    Cnum a = sel_cnum(swap, b0, a0);
    Cnum b = sel_cnum(swap, a0, b0);
    /* Rotate the circle so that a starts at 0. b1 is b in that frame. */
    u64 dbase = b.base - a.base;
    Cnum b1 = {dbase, b.size};
    /* b wraps in that frame: either both its pieces meet a (take the smaller arc), or only the
     * piece that wrapped does, and it covers the start of a. */
    Cnum both = sel_cnum(a.size <= b.size, a, b);
    Cnum wrapped = {a.base, min_u64(a.size, b1.base + b1.size)};
    Cnum r_wrap = sel_cnum(b1.base <= a.size, both, wrapped);
    /* b does not wrap: it meets a when it starts inside it. */
    Cnum meet = {b.base, min_u64(a.size - dbase, b.size)};
    Cnum r_flat = sel_cnum(a.size >= b1.base, meet, CNUM_EMPTY);
    Cnum r = sel_cnum(cnum_urange_overflow(b1), r_wrap, r_flat);
    return sel_cnum((int)cnum_is_empty(a0) | (int)cnum_is_empty(b0), CNUM_EMPTY, r);
}

/* Every sum x + y with x in a and y in b. If the sizes add up to more than the circle: all. */
static Cnum cnum_add(Cnum a, Cnum b)
{
    Cnum sum = cnum_normalize((Cnum){a.base + b.base, a.size + b.size});
    return sel_cnum(a.size > UINT64_MAX - b.size, CNUM_UNBOUNDED, sum);
}

static Cnum cnum_negate(Cnum a)
{
    return cnum_normalize((Cnum){0 - (a.base + a.size), a.size});
}

/* True when every value of `smaller` is in `bigger`. */
static bool cnum_is_subset(Cnum bigger, Cnum smaller)
{
    /* Rotate so that `bigger` starts at 0. */
    smaller.base -= bigger.base;
    bool wraps = cnum_urange_overflow(smaller);
    bool fits = smaller.base + smaller.size <= bigger.size;
    return sel_u64(wraps, bigger.size == UINT64_MAX, fits);
}

/* The arc that holds every number a tnum can stand for. */
static Cnum cnum_from_tnum(Tnum t)
{
    u64 sign = (u64)INT64_MIN;
    /* The sign bit is set or unknown: the signed minimum is the sign bit with the known low bits. */
    Cnum signed_arc =
        cnum_from_srange((s64)(t.value | (t.mask & sign)), (s64)(t.value | (t.mask & ~sign)));
    Cnum unsigned_arc = cnum_from_urange(t.value, t.value | t.mask);
    return sel_cnum(((t.mask | t.value) & sign) != 0, signed_arc, unsigned_arc);
}

/* ---------------------------------------------------------------------------------------- */
/* Register and stack state                                                                 */
/* ---------------------------------------------------------------------------------------- */

typedef enum { NOT_INIT, SCALAR, PTR_MEM, PTR_STACK } RegType;

/*
 * A scalar is its tnum and its cnum. A pointer keeps the part that varies in the same two
 * fields: the offset from the start of the region, constants that were added to it included.
 * `precise` is set only on a saved state, when a later decision depended on this exact value.
 */
typedef struct {
    RegType type;
    Tnum var_off;
    Cnum r;
    bool precise;
} Reg;

static u64 reg_umin(const Reg *r)
{
    return cnum_umin(r->r);
}

static u64 reg_umax(const Reg *r)
{
    return cnum_umax(r->r);
}

static s64 reg_smin(const Reg *r)
{
    return cnum_smin(r->r);
}

static s64 reg_smax(const Reg *r)
{
    return cnum_smax(r->r);
}

static Reg scalar_known(u64 v)
{
    return (Reg){.type = SCALAR, .var_off = tnum_const(v), .r = cnum_from_urange(v, v)};
}

/* A number that is unknown except that it fits in `bytes` bytes (a zero extended load). */
static Reg scalar_unknown(int bytes)
{
    u64 max = sel_u64(bytes == 8, UINT64_MAX, (1ULL << ((bytes * 8) & 63)) - 1);
    return (Reg){.type = SCALAR, .var_off = {0, max}, .r = cnum_from_urange(0, max)};
}

/* The tnum and the cnum each know things the other does not. Share them until stable. */
static void reg_bounds_sync(Reg *r)
{
    r->r = cnum_intersect(r->r, cnum_from_tnum(r->var_off));
    r->var_off = tnum_intersect(r->var_off, tnum_range(reg_umin(r), reg_umax(r)));
    r->r = cnum_intersect(r->r, cnum_from_tnum(r->var_off));
}

/* The same value, ignoring the precision mark. */
static bool reg_same(const Reg *a, const Reg *b)
{
    return (int)(a->type == b->type) & (int)(a->var_off.value == b->var_off.value) &
           (int)(a->var_off.mask == b->var_off.mask) & (int)(a->r.base == b->r.base) &
           (int)(a->r.size == b->r.size);
}

/* ---------------------------------------------------------------------------------------- */
/* Instructions                                                                             */
/* ---------------------------------------------------------------------------------------- */

enum { K_ALU, K_LDX, K_STX, K_JA, K_JMP, K_EXIT };
enum { ALU_MOV, ALU_ADD, ALU_SUB, ALU_MUL, ALU_AND, ALU_OR, ALU_XOR, ALU_LSH, ALU_RSH };
enum { J_EQ, J_NE, J_GT, J_GE, J_LT, J_LE, J_SGT, J_SGE, J_SLT, J_SLE, J_SET };

/*
 * `off` is the displacement of a load or store, or the jump distance counted from the next
 * instruction. `imm` is the constant operand when use_imm is set, and `src` the register
 * operand otherwise.
 */
typedef struct {
    int cls, op, dst, src, size;
    s64 off, imm;
    bool use_imm;
} Insn;

#define MOVK(d, k) {K_ALU, ALU_MOV, d, 0, 0, 0, k, true}
#define MOVX(d, s) {K_ALU, ALU_MOV, d, s, 0, 0, 0, false}
#define ALUK(o, d, k) {K_ALU, o, d, 0, 0, 0, k, true}
#define ALUX(o, d, s) {K_ALU, o, d, s, 0, 0, 0, false}
#define LDX(d, s, off, sz) {K_LDX, 0, d, s, sz, off, 0, false}
#define STX(d, off, s, sz) {K_STX, 0, d, s, sz, off, 0, false}
#define JA(off) {K_JA, 0, 0, 0, 0, off, 0, false}
#define JK(o, d, k, off) {K_JMP, o, d, 0, 0, off, k, true}
#define JX(o, d, s, off) {K_JMP, o, d, s, 0, off, 0, false}
#define EXIT {K_EXIT, 0, 0, 0, 0, 0, 0, false}

typedef struct {
    const char *name;
    int mem_size;
    int len;
    Insn insns[MAX_INSNS];
} Example;

#define EX(nm, msz, ...) \
    {nm, msz, (int)(sizeof((Insn[]){__VA_ARGS__}) / sizeof(Insn)), {__VA_ARGS__}}

/* Every example is one program. The expected verdict is what a concrete oracle says (see
 * FIDELITY.md), so a rejected program is one that can misbehave for some input. */
static const Example examples[] = {
    /* r2 is a byte. Masked to 0..15, it indexes a 16 byte buffer: the verifier proves it. */
    EX("bounded-index", 16, MOVK(0, 0), LDX(2, 1, 0, 1), ALUK(ALU_AND, 2, 15), MOVX(4, 1),
       ALUX(ALU_ADD, 4, 2), LDX(0, 4, 0, 1), EXIT),
    /* An unsigned compare gives the index an upper bound on the path that reaches the load. */
    EX("compare-guard", 16, MOVK(0, 0), LDX(2, 1, 0, 1), JK(J_GT, 2, 15, 4), MOVX(4, 1),
       ALUX(ALU_ADD, 4, 2), LDX(0, 4, 0, 1), EXIT, EXIT),
    /* An index that wraps below zero: two signed compares cut an arc that crosses 0. */
    EX("signed-guard", 16, MOVK(0, 0), LDX(2, 1, 0, 1), ALUK(ALU_SUB, 2, 8), JK(J_SLT, 2, 0, 4),
       JK(J_SGT, 2, 7, 3), MOVX(4, 1), ALUX(ALU_ADD, 4, 2), LDX(0, 4, 0, 1), EXIT),
    /* The compare of two constants has a known outcome: the code it skips is never verified. */
    EX("dead-branch", 16, MOVK(0, 0), MOVK(2, 5), MOVK(3, 9), JX(J_LT, 2, 3, 2), LDX(0, 1, 100, 1),
       EXIT, EXIT),
    /* Two paths join with different values of r3, which nothing depends on: the second path is
     * pruned. */
    EX("prune-imprecise", 16, MOVK(0, 0), LDX(2, 1, 0, 1), JK(J_GT, 2, 7, 2), MOVK(3, 1), JA(1),
       MOVK(3, 2), MOVX(0, 3), EXIT),
    /* The same join, but r3 is then added to a pointer: its value matters, so no pruning. */
    EX("prune-precise", 16, MOVK(0, 0), LDX(2, 1, 0, 1), JK(J_GT, 2, 7, 2), MOVK(3, 1), JA(1),
       MOVK(3, 2), MOVX(4, 1), ALUX(ALU_ADD, 4, 3), LDX(0, 4, 0, 1), EXIT),
    /* One path leaves r3 unwritten: a saved state that has it does not match. */
    EX("prune-uninit", 16, MOVK(0, 0), LDX(2, 1, 0, 1), JK(J_GT, 2, 7, 2), MOVK(3, 1), JA(1),
       MOVK(0, 1), EXIT),
    /* One path spills r3 and the other does not: a saved state with the slot does not match. */
    EX("prune-stack", 16, MOVK(0, 0), LDX(2, 1, 0, 1), JK(J_GT, 2, 7, 3), MOVK(3, 1),
       STX(10, -8, 3, 8), JA(1), MOVK(3, 2), EXIT),
    /* A loop that is verified by running it: the counter is a constant on every iteration. */
    EX("counter-loop", 16, MOVK(0, 0), MOVK(2, 0), MOVK(3, 0), MOVX(4, 1), ALUX(ALU_ADD, 4, 2),
       STX(4, 0, 3, 1), ALUK(ALU_ADD, 2, 1), JK(J_NE, 2, 16, -5), EXIT),
    /* The index is spilled to the stack and filled again: precision follows it through the slot. */
    EX("stack-spill", 16, MOVK(0, 0), LDX(2, 1, 0, 1), ALUK(ALU_AND, 2, 7), STX(10, -8, 2, 8),
       LDX(3, 1, 1, 1), ALUK(ALU_AND, 3, 3), STX(10, -16, 3, 8), JK(J_GT, 3, 1, 1), MOVK(0, 1), LDX(5, 10, -8, 8),
       MOVX(4, 1), ALUX(ALU_ADD, 4, 5), LDX(0, 4, 0, 1), EXIT),
    /* Multiplication of tnums: a known 0 bit, an unknown bit and a known 1 bit. */
    EX("scaled-index", 16, MOVK(0, 0), LDX(2, 1, 0, 1), ALUK(ALU_AND, 2, 3), MOVK(3, 2),
       ALUX(ALU_MUL, 3, 2), LDX(5, 1, 1, 1), ALUK(ALU_AND, 5, 3), ALUK(ALU_OR, 5, 1),
       ALUX(ALU_MUL, 5, 3), MOVX(4, 1), ALUX(ALU_ADD, 4, 3), LDX(0, 4, 0, 1), EXIT),
    /* Every ALU instruction on values that overflow, and a shift by an unknown amount. */
    EX("scalar-ops", 16, MOVK(0, 0), LDX(2, 1, 0, 4), LDX(3, 1, 4, 1), LDX(7, 1, 8, 8),
       MOVX(4, 2), ALUX(ALU_MUL, 4, 4), MOVX(5, 2), ALUX(ALU_ADD, 5, 5), ALUX(ALU_MUL, 5, 5),
       MOVX(6, 3), ALUK(ALU_LSH, 6, 3), ALUK(ALU_RSH, 6, 1), ALUK(ALU_LSH, 2, 32),
       ALUX(ALU_LSH, 2, 3), ALUX(ALU_OR, 4, 3), ALUX(ALU_XOR, 4, 3), ALUX(ALU_SUB, 4, 3), EXIT),
    /* Every kind of conditional jump whose outcome is not known, in both forms. */
    EX("refine-matrix", 8, MOVK(0, 0), LDX(2, 1, 0, 1), MOVX(3, 2), ALUK(ALU_SUB, 3, 128),
       JK(J_EQ, 2, 7, 13), JK(J_GT, 2, 200, 12), JK(J_LT, 2, 3, 11), JK(J_GE, 2, 190, 10),
       JK(J_LE, 2, 4, 9), JK(J_SGT, 3, 100, 8), JK(J_SLT, 3, -100, 7), JK(J_SGE, 3, 90, 6),
       JK(J_SLE, 3, -90, 5), JK(J_NE, 3, 50, 4), JX(J_GT, 2, 3, 3), JX(J_EQ, 2, 3, 2),
       JK(J_SET, 2, 1, 1), JK(J_SET, 2, 6, 0), EXIT),

    /* Rejected: r0 is not written before the exit. */
    EX("exit-without-r0", 16, EXIT),
    /* Rejected: arithmetic on a pointer other than adding a scalar. */
    EX("pointer-multiply", 16, MOVK(0, 0), MOVX(2, 1), ALUK(ALU_MUL, 2, 2), EXIT),
    /* Rejected: a pointer in a comparison. */
    EX("pointer-compare", 16, MOVK(0, 0), JK(J_EQ, 1, 0, 1), MOVK(0, 1), EXIT),
    /* Rejected: a load through a number. */
    EX("scalar-deref", 16, MOVK(0, 0), MOVK(2, 5), LDX(0, 2, 0, 1), EXIT),
    /* Rejected: the upper compare is signed, so a negative index gets through. */
    EX("negative-index", 16, MOVK(0, 0), LDX(2, 1, 0, 1), ALUK(ALU_SUB, 2, 8), JK(J_SGT, 2, 7, 3),
       MOVX(4, 1), ALUX(ALU_ADD, 4, 2), LDX(0, 4, 0, 1), EXIT),
    /* Rejected: a constant offset past the end. */
    EX("fixed-out-of-bounds", 16, MOVK(0, 0), LDX(0, 1, 16, 1), EXIT),
    /* Rejected: a four byte index has no useful upper bound. */
    EX("unbounded-index", 16, MOVK(0, 0), LDX(2, 1, 0, 4), MOVX(4, 1), ALUX(ALU_ADD, 4, 2),
       LDX(0, 4, 0, 1), EXIT),
    /* Rejected: the index is bounded, but not by the size of the buffer. */
    EX("variable-out-of-bounds", 16, MOVK(0, 0), LDX(2, 1, 0, 1), ALUK(ALU_AND, 2, 31), MOVX(4, 1),
       ALUX(ALU_ADD, 4, 2), LDX(0, 4, 0, 1), EXIT),
    /* Rejected: below the 64 bytes of the stack. */
    EX("stack-out-of-range", 16, MOVK(0, 0), MOVK(2, 7), STX(10, -72, 2, 8), EXIT),
    /* Rejected: a slot that was never written. */
    EX("stack-uninit-read", 16, MOVK(0, 0), LDX(0, 10, -8, 8), EXIT),
    /* Rejected: a stack pointer that moved by an amount the verifier does not know. */
    EX("stack-variable-index", 16, MOVK(0, 0), LDX(2, 1, 0, 1), ALUK(ALU_AND, 2, 7), MOVX(4, 10),
       ALUX(ALU_ADD, 4, 2), STX(4, -8, 0, 8), EXIT),
    /* Rejected: a jump to itself reaches the same state again. */
    EX("infinite-loop", 16, MOVK(0, 0), JA(-1)),
    /* Rejected: a loop that ends, but not within the limit on processed instructions. */
    EX("endless-counter", 16, MOVK(0, 0), MOVK(2, 0), ALUK(ALU_ADD, 2, 1), JK(J_NE, 2, 1000, -2),
       EXIT),
    /* Rejected: the loop writes one byte past the buffer on its last iteration. */
    EX("loop-overrun", 16, MOVK(0, 0), MOVK(2, 0), MOVK(3, 0), MOVX(4, 1), ALUX(ALU_ADD, 4, 2),
       STX(4, 0, 3, 1), ALUK(ALU_ADD, 2, 1), JK(J_NE, 2, 17, -5), EXIT),
};

/* ---------------------------------------------------------------------------------------- */
/* Verifier state                                                                           */
/* ---------------------------------------------------------------------------------------- */

/* One executed instruction of a path segment, and the stack slot it touched (or -1). */
typedef struct {
    int idx, slot;
} Hist;

typedef struct State State;
struct State {
    Reg regs[NREGS];
    Reg stack[STACK_SLOTS]; /* a slot holds a spilled register, or type NOT_INIT */
    State *parent;          /* the state saved at the start of the segment this one extends */
    int insn_idx, prev_insn_idx;
    int branches; /* children that have not finished yet */
    Hist hist[MAX_INSNS];
    int hist_cnt;
    State *next; /* the list of saved states of one instruction, or the stack of pending ones */
};

typedef struct {
    const Example *ex;
    bool prune[MAX_INSNS + 1];
    State states[MAX_STATES];
    int nstates;
    State *explored[MAX_INSNS];
    State *pending;
    State *cur;
    int processed;
    unsigned bt_regs, bt_stack;
    bool line_open;
    char notes[4096]; /* what the precision walk marked, printed after the instruction's line */
    size_t nnotes;
} Env;

static Env env;

static int fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static int fail(const char *fmt, ...)
{
    va_list ap;
    if (env.line_open) {
        fputc('\n', stderr);
        env.line_open = false;
    }
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return -1;
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    env.nnotes += (size_t)vsnprintf(env.notes + env.nnotes, sizeof env.notes - env.nnotes, fmt, ap);
    va_end(ap);
}

static void flush_notes(void)
{
    fputs(env.notes, stderr);
    env.notes[0] = '\0';
    env.nnotes = 0;
}

static State *new_state(void)
{
    return &env.states[env.nstates++];
}

/* ---------------------------------------------------------------------------------------- */
/* Printing                                                                                 */
/* ---------------------------------------------------------------------------------------- */

static const char *const alu_str[] = {"=", "+=", "-=", "*=", "&=", "|=", "^=", "<<=", ">>="};
static const char *const jmp_str[] = {"==", "!=", ">", ">=", "<", "<=", "s>", "s>=", "s<", "s<=", "&"};
static const char *const size_str[] = {"", "u8", "u16", "", "u32", "", "", "", "u64"};

static long long iabs(s64 v)
{
    return (long long)(v * (1 - 2 * (s64)(v < 0)));
}

static void disasm(char *buf, size_t n, const Insn *in)
{
    char opnd[32];
    snprintf(opnd, sizeof opnd, in->use_imm ? "%lld" : "r%lld", (long long)(in->use_imm ? in->imm : in->src));
    const char *sign = &"+-"[(int)(in->off < 0)];
    switch (in->cls) {
    case K_ALU:
        snprintf(buf, n, "r%d %s %s", in->dst, alu_str[in->op], opnd);
        break;
    case K_LDX:
        snprintf(buf, n, "r%d = *(%s *)(r%d %.1s %lld)", in->dst, size_str[in->size], in->src, sign,
                 iabs(in->off));
        break;
    case K_STX:
        snprintf(buf, n, "*(%s *)(r%d %.1s %lld) = r%d", size_str[in->size], in->dst, sign,
                 iabs(in->off), in->src);
        break;
    case K_JA:
        snprintf(buf, n, "goto %.1s%lld", sign, iabs(in->off));
        break;
    case K_JMP:
        snprintf(buf, n, "if r%d %s %s goto %.1s%lld", in->dst, jmp_str[in->op], opnd, sign,
                 iabs(in->off));
        break;
    default:
        snprintf(buf, n, "exit");
        break;
    }
}

/* A register as the kernel's log shows it: a constant, or the facts that are not the default. */
static void print_reg(const Reg *r)
{
    static const char *const names[] = {"", "scalar", "mem", "fp"};
    fputs(&"P"[(int)!r->precise], stderr);
    if (tnum_is_const(r->var_off)) {
        if (r->type != SCALAR) {
            fprintf(stderr, "%s%+lld", names[r->type], (long long)r->var_off.value);
            return;
        }
        fprintf(stderr, "%lld", (long long)r->var_off.value);
        return;
    }
    const char *sep = "";
    fprintf(stderr, "%s(", names[r->type]);
    if (reg_umin(r) != 0) {
        fprintf(stderr, "%sumin=%llu", sep, (unsigned long long)reg_umin(r));
        sep = ",";
    }
    if (reg_umax(r) != UINT64_MAX) {
        fprintf(stderr, "%sumax=%llu", sep, (unsigned long long)reg_umax(r));
        sep = ",";
    }
    if (reg_smin(r) != INT64_MIN) {
        fprintf(stderr, "%ssmin=%lld", sep, (long long)reg_smin(r));
        sep = ",";
    }
    if (reg_smax(r) != INT64_MAX) {
        fprintf(stderr, "%ssmax=%lld", sep, (long long)reg_smax(r));
        sep = ",";
    }
    if ((int)(r->var_off.value != 0) | (int)(r->var_off.mask != UINT64_MAX)) {
        fprintf(stderr, "%svar_off=(%#llx; %#llx)", sep, (unsigned long long)r->var_off.value,
                (unsigned long long)r->var_off.mask);
    }
    fprintf(stderr, ")");
}

static void print_slot_reg(int slot, const Reg *r)
{
    fprintf(stderr, " fp-%d=", 64 - 8 * slot);
    print_reg(r);
}

static void print_state(const State *s)
{
    for (int r = 0; r < NREGS; r++) {
        if (s->regs[r].type != NOT_INIT) {
            fprintf(stderr, " R%d=", r);
            print_reg(&s->regs[r]);
        }
    }
    for (int i = 0; i < STACK_SLOTS; i++) {
        if (s->stack[i].type != NOT_INIT) {
            print_slot_reg(i, &s->stack[i]);
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Precision: walking a path backwards (kernel/bpf/backtrack.c)                             */
/* ---------------------------------------------------------------------------------------- */

/*
 * Walk one instruction backwards, updating the set of registers and stack slots whose value at
 * that point must be exact. A register in the set that the instruction computed from others
 * hands the requirement on to them.
 */
static void backtrack_insn(const Insn *in, int slot)
{
    unsigned d = 1u << in->dst, s = 1u << in->src;
    unsigned src_bit = s * (unsigned)!in->use_imm;
    switch (in->cls) {
    case K_ALU:
        if (!(env.bt_regs & d)) {
            return;
        }
        env.bt_regs &= ~(d * (unsigned)(in->op == ALU_MOV));
        env.bt_regs |= src_bit;
        return;
    case K_LDX:
        if (!(env.bt_regs & d)) {
            return;
        }
        /* The loaded value is not a function of anything earlier, except through a stack slot. */
        env.bt_regs &= ~d;
        if (slot >= 0) {
            env.bt_stack |= 1u << slot;
        }
        return;
    case K_STX:
        if (slot < 0) {
            return;
        }
        if (!(env.bt_stack & (1u << slot))) {
            return;
        }
        env.bt_stack &= ~(1u << slot);
        env.bt_regs |= s;
        return;
    case K_JMP:
        /* A jump decided by two registers needed both of them. */
        if (!(env.bt_regs & (d | src_bit))) {
            return;
        }
        env.bt_regs |= d | src_bit;
        return;
    default: /* goto and exit change nothing */
        return;
    }
}

/* Mark what the requested set holds as precise in the saved state `st`. */
static void mark_precise(State *st)
{
    for (int r = 0; r < NREGS; r++) {
        unsigned bit = (env.bt_regs >> r) & 1;
        unsigned was = st->regs[r].precise;
        if (bit & ~was) {
            note("    r%d is precise in the state saved at insn %d\n", r, st->insn_idx);
        }
        /* A register that is already precise here was walked back from before: stop asking. */
        env.bt_regs &= ~((bit & was) << r);
        st->regs[r].precise = (bool)(bit | was);
    }
    for (int i = 0; i < STACK_SLOTS; i++) {
        unsigned bit = (env.bt_stack >> i) & 1;
        unsigned was = st->stack[i].precise;
        if (bit & ~was) {
            note("    fp-%d is precise in the state saved at insn %d\n", 64 - 8 * i, st->insn_idx);
        }
        env.bt_stack &= ~((bit & was) << i);
        st->stack[i].precise = (bool)(bit | was);
    }
}

/*
 * The current path is the segment since the last saved state, then that state's own segment, and
 * so on up its parents. Walk each segment backwards over its history, then mark what is still
 * asked for in the state that starts it. `skip` is 1 when the instruction that asks has already
 * left its entry in the history.
 */
static void walk_precision(int skip)
{
    State *st = env.cur;
    for (;;) {
        for (int k = st->hist_cnt - 1 - skip; k >= 0; k--) {
            backtrack_insn(&env.ex->insns[st->hist[k].idx], st->hist[k].slot);
        }
        skip = 0;
        st = st->parent;
        if (!st) {
            return;
        }
        mark_precise(st);
        if ((env.bt_regs | env.bt_stack) == 0) {
            return;
        }
    }
}

/* The instruction being executed decides on the value of one register. */
static void mark_precision(int regno)
{
    env.bt_regs = 1u << regno;
    env.bt_stack = 0;
    walk_precision(1);
}

/* A pruned path inherits the precision of the state that made it safe. */
static void propagate_precision(const State *old)
{
    env.bt_regs = 0;
    env.bt_stack = 0;
    for (int r = 0; r < NREGS; r++) {
        env.bt_regs |= (unsigned)old->regs[r].precise << r;
    }
    for (int i = 0; i < STACK_SLOTS; i++) {
        env.bt_stack |= (unsigned)old->stack[i].precise << i;
    }
    walk_precision(0);
}

/* ---------------------------------------------------------------------------------------- */
/* Scalar arithmetic (adjust_scalar_min_max_vals)                                           */
/* ---------------------------------------------------------------------------------------- */

static void scalar_shift(Reg *d, const Reg *s, int op)
{
    bool known = (int)tnum_is_const(s->var_off) & (int)(s->var_off.value < 64);
    if (!known) {
        *d = scalar_unknown(8);
        return;
    }
    int n = (int)(s->var_off.value & 63);
    u64 umin = reg_umin(d), umax = reg_umax(d);
    if (op == ALU_LSH) {
        d->var_off = tnum_lshift(d->var_off, n);
        /* Only a result that stays below 2^63 keeps its bounds, signed and unsigned. */
        d->r = sel_cnum(umax <= (1ULL << (63 - n)), cnum_from_urange(umin << n, umax << n),
                        CNUM_UNBOUNDED);
        return;
    }
    d->var_off = tnum_rshift(d->var_off, n);
    d->r = cnum_from_urange(umin >> n, umax >> n);
}

static void scalar_mul(Reg *d, const Reg *s)
{
    u64 dumin = reg_umin(d), dumax = reg_umax(d), sumin = reg_umin(s), sumax = reg_umax(s);
    s128 a = reg_smin(d), b = reg_smax(d), c = reg_smin(s), e = reg_smax(s);
    s128 lo = min_s128(min_s128(a * c, a * e), min_s128(b * c, b * e));
    s128 hi = max_s128(max_s128(a * c, a * e), max_s128(b * c, b * e));
    d->var_off = tnum_mul(d->var_off, s->var_off);
    /* A product that does not fit in 64 bits says nothing: unsigned and signed each decide. */
    Cnum u = sel_cnum((u128)dumax * sumax <= UINT64_MAX,
                      cnum_from_urange(dumin * sumin, dumax * sumax), CNUM_UNBOUNDED);
    Cnum sg = sel_cnum((int)(lo >= INT64_MIN) & (int)(hi <= INT64_MAX),
                       cnum_from_srange((s64)lo, (s64)hi), CNUM_UNBOUNDED);
    d->r = cnum_intersect(u, sg);
}

/* d = d op s, with both scalars (or d a pointer and the op an add). */
static void scalar_alu(Reg *d, const Reg *s, int op)
{
    u64 dumin = reg_umin(d), dumax = reg_umax(d), sumin = reg_umin(s), sumax = reg_umax(s);
    switch (op) {
    case ALU_ADD:
        d->var_off = tnum_add(d->var_off, s->var_off);
        d->r = cnum_add(d->r, s->r);
        break;
    case ALU_SUB:
        d->var_off = tnum_sub(d->var_off, s->var_off);
        d->r = cnum_add(d->r, cnum_negate(s->r));
        break;
    case ALU_MUL:
        scalar_mul(d, s);
        break;
    case ALU_AND:
        /* The tnum goes first: the bounds below read it. */
        d->var_off = tnum_and(d->var_off, s->var_off);
        d->r = cnum_from_urange(d->var_off.value, min_u64(dumax, sumax));
        break;
    case ALU_OR:
        d->var_off = tnum_or(d->var_off, s->var_off);
        d->r = cnum_from_urange(max_u64(dumin, sumin), d->var_off.value | d->var_off.mask);
        break;
    case ALU_XOR:
        d->var_off = tnum_xor(d->var_off, s->var_off);
        d->r = cnum_from_urange(d->var_off.value, d->var_off.value | d->var_off.mask);
        break;
    default:
        scalar_shift(d, s, op);
        break;
    }
    reg_bounds_sync(d);
}

/* ---------------------------------------------------------------------------------------- */
/* Conditional jumps: is the outcome known, and what does each outcome tell us              */
/* ---------------------------------------------------------------------------------------- */

/*
 * Eleven jump opcodes reduce to four tests with a swap of the operands and a negation of the
 * result: a > b is b < a, a >= b is not a < b, a <= b is not b < a. The kernel writes out every
 * case instead.
 */
enum { N_EQ, N_LT, N_SLT, N_SET };

typedef struct {
    int op, swap, neg;
} Jnorm;

static const Jnorm jnorm[] = {
    [J_EQ] = {N_EQ, 0, 0},  [J_NE] = {N_EQ, 0, 1},  [J_GT] = {N_LT, 1, 0},  [J_GE] = {N_LT, 0, 1},
    [J_LT] = {N_LT, 0, 0},  [J_LE] = {N_LT, 1, 1},  [J_SGT] = {N_SLT, 1, 0}, [J_SGE] = {N_SLT, 0, 1},
    [J_SLT] = {N_SLT, 0, 0}, [J_SLE] = {N_SLT, 1, 1}, [J_SET] = {N_SET, 0, 0},
};

/* 1 if the test is always true, 0 if it never is, -1 if the registers do not decide it. */
static int branch_taken(const Reg *a, const Reg *b, int op)
{
    int lt, ge;
    switch (op) {
    case N_EQ: {
        int konst = (int)tnum_is_const(a->var_off) & (int)tnum_is_const(b->var_off);
        int never = (int)!tnum_overlap(a->var_off, b->var_off) |
                    (int)cnum_is_empty(cnum_intersect(a->r, b->r));
        if (konst) {
            return !never;
        }
        return -(int)!never;
    }
    case N_LT:
        lt = reg_umax(a) < reg_umin(b);
        ge = reg_umin(a) >= reg_umax(b);
        return lt - (int)(!lt & !ge);
    case N_SLT:
        lt = reg_smax(a) < reg_smin(b);
        ge = reg_smin(a) >= reg_smax(b);
        return lt - (int)(!lt & !ge);
    default: /* N_SET: a & b is not 0 */
        lt = (a->var_off.value & b->var_off.value) != 0;
        ge = ((a->var_off.value | a->var_off.mask) & (b->var_off.value | b->var_off.mask)) == 0;
        return lt - (int)(!lt & !ge);
    }
}

/* x < y, unsigned. */
static void refine_lt(Reg *x, Reg *y)
{
    u64 xmin = reg_umin(x), ymax = reg_umax(y);
    x->r = cnum_intersect(x->r, cnum_from_urange(0, ymax - 1));
    y->r = cnum_intersect(y->r, cnum_from_urange(xmin + 1, UINT64_MAX));
}

/* x <= y, unsigned. */
static void refine_le(Reg *x, Reg *y)
{
    u64 xmin = reg_umin(x), ymax = reg_umax(y);
    x->r = cnum_intersect(x->r, cnum_from_urange(0, ymax));
    y->r = cnum_intersect(y->r, cnum_from_urange(xmin, UINT64_MAX));
}

static void refine_slt(Reg *x, Reg *y)
{
    s64 xmin = reg_smin(x), ymax = reg_smax(y);
    x->r = cnum_intersect(x->r, cnum_from_srange(INT64_MIN, (s64)((u64)ymax - 1)));
    y->r = cnum_intersect(y->r, cnum_from_srange((s64)((u64)xmin + 1), INT64_MAX));
}

static void refine_sle(Reg *x, Reg *y)
{
    s64 xmin = reg_smin(x), ymax = reg_smax(y);
    x->r = cnum_intersect(x->r, cnum_from_srange(INT64_MIN, ymax));
    y->r = cnum_intersect(y->r, cnum_from_srange(xmin, INT64_MAX));
}

/* x != y where y is a constant: it can only cut the end of the arc that x is. */
static void trim_ne(Reg *x, const Reg *y)
{
    u64 v = y->var_off.value;
    Cnum c = x->r;
    int konst = (int)tnum_is_const(y->var_off);
    int at_start = konst & (int)(c.base == v) & (int)(c.size > 0);
    int at_end = konst & (int)(c.base + c.size == v) & (int)(c.size > 0);
    x->r = (Cnum){c.base + (u64)at_start, c.size - (u64)(at_start | at_end)};
}

/*
 * a < b (or one of the other tests) is `truth`. Narrow both registers. The test on a bit mask
 * learns something only from a constant: the true branch of a single bit mask sets that bit, and
 * the false branch clears every bit of the mask.
 */
static void refine(Reg *a, Reg *b, int op, int truth)
{
    switch (op) {
    case N_EQ:
        if (truth) {
            Tnum t = tnum_intersect(a->var_off, b->var_off);
            Cnum c = cnum_intersect(a->r, b->r);
            a->var_off = b->var_off = t;
            a->r = b->r = c;
        } else {
            trim_ne(a, b);
            trim_ne(b, a);
        }
        break;
    case N_LT:
        if (truth) {
            refine_lt(a, b);
        } else {
            refine_le(b, a);
        }
        break;
    case N_SLT:
        if (truth) {
            refine_slt(a, b);
        } else {
            refine_sle(b, a);
        }
        break;
    default: {
        Reg *pr[2] = {b, a};
        int bc = (int)tnum_is_const(b->var_off);
        Reg *x = pr[bc], *k = pr[!bc];
        u64 val = k->var_off.value;
        int single = (val & (val - 1)) == 0;
        int apply = (int)tnum_is_const(k->var_off) & ((truth & single) | !truth);
        Tnum with = tnum_or(x->var_off, tnum_const(val));
        Tnum without = tnum_and(x->var_off, tnum_const(~val));
        x->var_off = sel_tnum(apply, sel_tnum(truth, with, without), x->var_off);
        break;
    }
    }
    reg_bounds_sync(a);
    reg_bounds_sync(b);
}

/* ---------------------------------------------------------------------------------------- */
/* Memory                                                                                   */
/* ---------------------------------------------------------------------------------------- */

/* The four ways the kernel finds a region access unsafe, in its order. */
static int check_mem_region(const Reg *p, s64 off, int size, int mem_size)
{
    s128 smin = reg_smin(p), umax = reg_umax(p);
    s128 lo = smin + off;
    if (smin < 0) {
        return fail("insn %d: the minimum of the index is %lld, it can be negative",
                    env.cur->insn_idx, (long long)smin);
    }
    if ((int)(lo < 0) | (int)(lo + size > mem_size)) {
        return fail("insn %d: the minimum offset %lld is outside the %d byte memory",
                    env.cur->insn_idx, (long long)lo, mem_size);
    }
    if (umax >= MAX_VAR_OFF) {
        return fail("insn %d: the maximum of the index is unbounded", env.cur->insn_idx);
    }
    if (umax + off + size > mem_size) {
        return fail("insn %d: the maximum offset %lld is outside the %d byte memory",
                    env.cur->insn_idx, (long long)(umax + off), mem_size);
    }
    return 0;
}

/* Only a fixed offset, 8 bytes wide and 8 aligned, from fp-64 to fp-8. Returns the slot. */
static int check_stack(const Reg *p, const Insn *in)
{
    s64 o = (s64)(p->var_off.value + (u64)in->off);
    int ok = (int)tnum_is_const(p->var_off) & (int)(o >= -64) & (int)(o <= -8) &
             (int)((o & 7) == 0) & (int)(in->size == 8);
    if (!ok) {
        return fail("insn %d: a stack access needs a fixed offset, 8 bytes wide and aligned, "
                    "from fp-64 to fp-8",
                    env.cur->insn_idx);
    }
    return (int)((o + 64) / 8);
}

static int do_mem(const Insn *in)
{
    State *st = env.cur;
    bool is_load = in->cls == K_LDX;
    int preg = is_load ? in->src : in->dst;
    const Reg *p = &st->regs[preg];
    if (p->type == SCALAR) {
        return fail("insn %d: r%d is a number, not a pointer", st->insn_idx, preg);
    }
    if (p->type == PTR_MEM) {
        if (check_mem_region(p, in->off, in->size, env.ex->mem_size) < 0) {
            return -1;
        }
        if (is_load) {
            st->regs[in->dst] = scalar_unknown(in->size);
        }
        return 0;
    }
    int slot = check_stack(p, in);
    if (slot < 0) {
        return -1;
    }
    st->hist[st->hist_cnt - 1].slot = slot;
    if (!is_load) {
        st->stack[slot] = st->regs[in->src];
        return 0;
    }
    if (st->stack[slot].type == NOT_INIT) {
        return fail("insn %d: fp-%d was never written", st->insn_idx, 64 - 8 * slot);
    }
    st->regs[in->dst] = st->stack[slot];
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* Instructions                                                                             */
/* ---------------------------------------------------------------------------------------- */

/* The registers an instruction reads. */
static unsigned insn_reads(const Insn *in)
{
    unsigned dst = 1u << in->dst, src = 1u << in->src;
    unsigned use_src = src * (unsigned)!in->use_imm;
    switch (in->cls) {
    case K_ALU:
        return dst * (unsigned)(in->op != ALU_MOV) | use_src;
    case K_LDX:
        return src;
    case K_STX:
        return dst | src;
    case K_JMP:
        return dst | use_src;
    case K_JA:
        return 0;
    default: /* exit returns r0 */
        return 1;
    }
}

static int do_alu(const Insn *in)
{
    State *st = env.cur;
    Reg *dst = &st->regs[in->dst];
    Reg src = scalar_known((u64)in->imm);
    if (!in->use_imm) {
        src = st->regs[in->src];
    }
    if (in->op == ALU_MOV) {
        *dst = src;
        return 0;
    }
    int bad = (int)(src.type != SCALAR) | ((int)(dst->type != SCALAR) & (int)(in->op != ALU_ADD));
    if (bad) {
        return fail("insn %d: a pointer can only get a number added to it", st->insn_idx);
    }
    /* What is added to a pointer decides every later access through it, so it must stay exact. */
    int ptr_add_reg = (int)(dst->type != SCALAR) & (int)!in->use_imm;
    if (ptr_add_reg) {
        mark_precision(in->src);
    }
    scalar_alu(dst, &src, in->op);
    return 0;
}

/* The state that explores the other side of a branch later. */
static State *push_branch(int target, int from)
{
    State *p = new_state();
    *p = *env.cur;
    p->branches = 0;
    p->insn_idx = target;
    p->prev_insn_idx = from;
    p->next = env.pending;
    env.pending = p;
    if (env.cur->parent) {
        env.cur->parent->branches++;
    }
    fprintf(stderr, " push->%d", target);
    return p;
}

static int do_jmp(const Insn *in, int idx, int *next)
{
    State *st = env.cur;
    Reg *dst = &st->regs[in->dst];
    Reg src = scalar_known((u64)in->imm);
    if (!in->use_imm) {
        src = st->regs[in->src];
    }
    if ((int)(dst->type != SCALAR) | (int)(src.type != SCALAR)) {
        return fail("insn %d: a pointer cannot be compared", idx);
    }
    const Jnorm *n = &jnorm[in->op];
    Reg d = *dst, s = src;
    Reg *pair[2] = {&d, &s};
    int taken = branch_taken(pair[n->swap], pair[!n->swap], n->op);
    taken ^= (int)(taken >= 0) & n->neg;
    if (taken >= 0) {
        /* The jump is decided by the values of its registers, so they must stay exact. */
        mark_precision(in->dst);
        if (!in->use_imm) {
            mark_precision(in->src);
        }
        *next = idx + 1 + taken * (int)in->off;
        return 0;
    }
    /* Both outcomes are possible: refine a copy of the operands for each. */
    Reg td = *dst, ts = src, fd = *dst, fs = src;
    Reg *tp[2] = {&td, &ts}, *fp[2] = {&fd, &fs};
    refine(tp[n->swap], tp[!n->swap], n->op, !n->neg);
    refine(fp[n->swap], fp[!n->swap], n->op, n->neg);
    State *pend = push_branch(idx + 1 + (int)in->off, idx);
    pend->regs[in->dst] = td;
    st->regs[in->dst] = fd;
    if (!in->use_imm) {
        pend->regs[in->src] = ts;
        st->regs[in->src] = fs;
    }
    return 0;
}

/* Run the instruction on the current state. Returns -1 on an error, 1 on exit, else 0. */
static int exec_insn(const Insn *in, int idx, int *next)
{
    State *st = env.cur;
    unsigned reads = insn_reads(in);
    *next = idx + 1;
    for (int r = 0; r < NREGS; r++) {
        int bad = (int)((reads >> r) & 1) & (int)(st->regs[r].type == NOT_INIT);
        if (bad) {
            return fail("insn %d: r%d is read before it is written", idx, r);
        }
    }
    switch (in->cls) {
    case K_ALU:
        return do_alu(in);
    case K_LDX:
    case K_STX:
        return do_mem(in);
    case K_JMP:
        return do_jmp(in, idx, next);
    case K_JA:
        *next = idx + 1 + (int)in->off;
        return 0;
    default:
        return 1;
    }
}

/* Execute one instruction and print it with the registers that it changed. */
static int run_insn(const Insn *in, int idx, int *next)
{
    State *st = env.cur;
    Reg regs[NREGS], stack[STACK_SLOTS];
    char text[96];
    memcpy(regs, st->regs, sizeof regs);
    memcpy(stack, st->stack, sizeof stack);
    disasm(text, sizeof text, in);
    fprintf(stderr, "%2d: %-34s;", idx, text);
    env.line_open = true;
    int r = exec_insn(in, idx, next);
    if (r < 0) {
        return -1;
    }
    for (int i = 0; i < NREGS; i++) {
        if (!reg_same(&regs[i], &st->regs[i])) {
            fprintf(stderr, " R%d=", i);
            print_reg(&st->regs[i]);
        }
    }
    for (int i = 0; i < STACK_SLOTS; i++) {
        if (!reg_same(&stack[i], &st->stack[i])) {
            print_slot_reg(i, &st->stack[i]);
        }
    }
    fputc('\n', stderr);
    env.line_open = false;
    flush_notes();
    return r;
}

/* ---------------------------------------------------------------------------------------- */
/* Saved states and pruning (kernel/bpf/states.c)                                           */
/* ---------------------------------------------------------------------------------------- */

/*
 * Is `cur` covered by the register `old` of a state that was fully explored? With `exact` the
 * two must be identical, which is how a state that loops back onto itself is told from one that
 * only resembles it.
 */
static bool regsafe(const Reg *old, const Reg *cur, bool exact)
{
    if (exact) {
        return reg_same(old, cur);
    }
    if (old->type == NOT_INIT) {
        return true;
    }
    if (old->type != cur->type) {
        return false;
    }
    /* A number that nothing depended on matches any number. */
    int imprecise = (int)(old->type == SCALAR) & (int)!old->precise;
    if (imprecise) {
        return true;
    }
    return (int)cnum_is_subset(old->r, cur->r) & (int)tnum_in(old->var_off, cur->var_off);
}

static bool states_equal(const State *old, const State *cur, bool exact)
{
    for (int r = 0; r < NREGS; r++) {
        if (!regsafe(&old->regs[r], &cur->regs[r], exact)) {
            return false;
        }
    }
    for (int i = 0; i < STACK_SLOTS; i++) {
        if (!regsafe(&old->stack[i], &cur->stack[i], exact)) {
            return false;
        }
    }
    return true;
}

/* Returns -1 for a loop, 1 if the current state is pruned, 0 if it goes on (and is saved). */
static int is_state_visited(int idx)
{
    State *st = env.cur;
    if (!env.prune[idx]) {
        return 0;
    }
    for (State *sl = env.explored[idx]; sl; sl = sl->next) {
        if (sl->branches) {
            /* A state that is still being explored is an ancestor: arriving at it unchanged
             * means the program can run forever. */
            if (states_equal(sl, st, true)) {
                return fail("infinite loop detected at insn %d", idx);
            }
            continue;
        }
        if (states_equal(sl, st, false)) {
            fprintf(stderr, "%2d: safe, covered by the state saved earlier:", idx);
            print_state(sl);
            fputc('\n', stderr);
            propagate_precision(sl);
            flush_notes();
            return 1;
        }
    }
    State *s = new_state();
    *s = *st;
    s->insn_idx = idx;
    s->branches = 1;
    s->next = env.explored[idx];
    env.explored[idx] = s;
    st->parent = s;
    st->hist_cnt = 0;
    fprintf(stderr, "%2d: state saved\n", idx);
    return 0;
}

/* The current path has ended: a saved state whose paths have all ended is explored. */
static void end_path(void)
{
    for (State *s = env.cur->parent; s; s = s->parent) {
        s->branches--;
        if (s->branches) {
            break;
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* The stages                                                                               */
/* ---------------------------------------------------------------------------------------- */

static void stage_source(void)
{
    char text[96];
    fprintf(stderr, "== stage_source ==\n");
    fprintf(stderr, "r1 points to %d bytes of memory, r10 to the end of a 64 byte stack\n",
            env.ex->mem_size);
    for (int i = 0; i < env.ex->len; i++) {
        disasm(text, sizeof text, &env.ex->insns[i]);
        fprintf(stderr, "%2d: %s\n", i, text);
    }
    fprintf(stderr, "\n");
}

static void stage_cfg(void)
{
    fprintf(stderr, "== stage_cfg ==\n");
    for (int i = 0; i < env.ex->len; i++) {
        const Insn *in = &env.ex->insns[i];
        int is_jmp = in->cls == K_JMP, is_ja = in->cls == K_JA;
        int target = i + 1 + (int)in->off;
        if (is_ja) {
            env.prune[target] = true;
            fprintf(stderr, "%2d: goes to %d\n", i, target);
        }
        if (is_jmp) {
            env.prune[target] = true;
            env.prune[i + 1] = true;
            fprintf(stderr, "%2d: goes to %d or on to %d\n", i, target, i + 1);
        }
    }
    fprintf(stderr, "prune points:");
    for (int i = 0; i < env.ex->len; i++) {
        if (env.prune[i]) {
            fprintf(stderr, " %d", i);
        }
    }
    fprintf(stderr, "\n\n");
}

/* The depth first search. Returns 0 if every path is safe, -1 at the first error. */
static int do_check(void)
{
    State *st = new_state();
    env.cur = st;
    st->prev_insn_idx = -1;
    st->regs[1] = (Reg){.type = PTR_MEM, .var_off = tnum_const(0), .r = cnum_from_urange(0, 0)};
    st->regs[R_FP] = (Reg){.type = PTR_STACK, .var_off = tnum_const(0), .r = cnum_from_urange(0, 0)};
    fprintf(stderr, "start:");
    print_state(st);
    fputc('\n', stderr);
    for (;;) {
        st = env.cur;
        int idx = st->insn_idx;
        if (++env.processed > COMPLEXITY_LIMIT) {
            return fail("the program is too complex: more than %d instructions processed",
                        COMPLEXITY_LIMIT);
        }
        int next = idx + 1;
        int done = is_state_visited(idx);
        if (done < 0) {
            return -1;
        }
        if (!done) {
            st->hist[st->hist_cnt++] = (Hist){idx, -1};
            done = run_insn(&env.ex->insns[idx], idx, &next);
            if (done < 0) {
                return -1;
            }
        }
        if (!done) {
            st->prev_insn_idx = idx;
            st->insn_idx = next;
            continue;
        }
        end_path();
        if (!env.pending) {
            return 0;
        }
        env.cur = env.pending;
        env.pending = env.cur->next;
        fprintf(stderr, "from %d to %d:", env.cur->prev_insn_idx, env.cur->insn_idx);
        print_state(env.cur);
        fputc('\n', stderr);
    }
}

static int stage_verify(void)
{
    fprintf(stderr, "== stage_verify ==\n");
    int rc = do_check();
    fprintf(stderr, "processed %d instructions\n", env.processed);
    return rc;
}

int main(int argc, char **argv)
{
    const Example *ex = NULL;
    for (size_t i = 0; argc >= 2 && i < sizeof examples / sizeof examples[0]; i++) {
        if (strcmp(argv[1], examples[i].name) == 0) {
            ex = &examples[i];
        }
    }
    if (ex == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    env.ex = ex;
    stage_source();
    stage_cfg();
    int rc = stage_verify();
    puts((const char *const[]){"accept", "reject"}[rc < 0]);
    return 0;
}
