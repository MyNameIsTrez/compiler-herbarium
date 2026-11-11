/*
 * go-ssa: a cut-down version of the register allocator of the Go compiler
 * (cmd/compile/internal/ssa/regalloc.go).
 *
 * Go does not build an interference graph and color it. It treats the whole function as one long
 * basic block and walks through it once, greedily: a value is moved into a register just before
 * its use, and when no register is free the one whose value is used farthest in the future is
 * kicked out. Each block starts from the register state at the end of one of its predecessors,
 * so a block with several predecessors disagrees with the others, and a second pass over the
 * merge edges (`shuffle`) adds the moves that make the states agree. That is only possible
 * because the edge's source has one successor, so the moves go at the end of the source block.
 *
 * A value that is kicked out and used later gets a spill (a store) and a restore (a load). The
 * walk does not know where the store should go, so it only records which part of the dominator
 * tree contains restores. Afterwards the store is placed in the highest block that dominates all
 * restores and has the value in a register, which is often much later than the definition.
 * Constants are not spilled: they are issued again before each use.
 *
 * The machine has NREGS registers and every call kicks all of them out, which is what makes
 * values live across calls interesting. The input is the SSA of each example after the generic
 * passes: a function per example builds it, with the blocks already in order and no critical
 * edges, as `critical`, `layout` and `schedule` would leave it.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. The allocated code is run by an interpreter, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source   show the Lua source of the example
 *   stage_build    build the SSA of each function and show it
 *   stage_live     compute how far away the next use of every value is at the end of each block
 *   stage_regalloc the greedy walk over the blocks, with the values in registers
 *   stage_spills   place each spill in the dominator tree and give the spills stack slots
 *   stage_shuffle  fix up the merge edges
 *   stage_run      run the allocated code on a machine with NREGS registers
 */
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NREGS 2
#define ALL_REGS ((1u << NREGS) - 1)
#define R0 1u
#define BIT(r) (1u << (r))

#define MAX_FUNCS 2
#define MAX_BLOCKS 8
#define MAX_VALUES 96
#define MAX_BVALS 40
#define MAX_PREDS 2
#define MAX_USES 8
#define MAX_SLOTS 8
#define MAX_LOCS (NREGS + MAX_SLOTS)
#define MAX_DESTS 8
#define NO_REG (-1)
#define NO_HOME (-1)
#define POISON 0x5eadbeefL

/* How far away a use is, in instructions. */
#define LIKELY_DISTANCE 1
#define NORMAL_DISTANCE 10
#define UNLIKELY_DISTANCE 100

typedef unsigned regmask;

static int lowest(regmask m)
{
    return __builtin_ctz(m);
}

static int count_regs(regmask m)
{
    return __builtin_popcount(m);
}

/* ------------------------------------------------------------------------------------------
 * The SSA form
 * ---------------------------------------------------------------------------------------- */

enum op {
    OP_CONST,    /* aux: the number */
    OP_ARG,      /* the argument, which arrives in R0 */
    OP_PHI,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_LESS,     /* 1 if the first is less than the second, else 0 */
    OP_CALL,     /* aux: the function; the argument goes in R0 and so does the result */
    OP_PRINT,    /* print the number in R0 */
    OP_PRINTSTR, /* aux: the string */
    OP_COPY,     /* made by the allocator: register to register */
    OP_STORE,    /* made by the allocator: register to stack slot (a spill) */
    OP_LOAD      /* made by the allocator: stack slot to register (a restore) */
};

#define OP_COUNT (OP_LOAD + 1)

#define F_REMAT 1       /* cheap to compute again, so never spilled */
#define F_COMMUTATIVE 2
#define F_ARG0 4        /* the result goes in the register of the first argument (x86) */
#define F_VOID 8        /* has no result */
#define F_CALL 16

struct opinfo {
    const char *name;
    int nargs;
    regmask in[2]; /* the registers each argument may be in */
    regmask out;   /* the registers the result may be in, 0 for none */
    regmask clobbers;
    unsigned flags;
};

static const struct opinfo ops[OP_COUNT] = {
    [OP_CONST] = {"CONST", 0, {0, 0}, ALL_REGS, 0, F_REMAT},
    [OP_ARG] = {"ARG", 0, {0, 0}, R0, 0, 0},
    [OP_PHI] = {"PHI", 2, {0, 0}, ALL_REGS, 0, 0},
    [OP_ADD] = {"ADD", 2, {ALL_REGS, ALL_REGS}, ALL_REGS, 0, F_COMMUTATIVE | F_ARG0},
    [OP_SUB] = {"SUB", 2, {ALL_REGS, ALL_REGS}, ALL_REGS, 0, F_ARG0},
    [OP_MUL] = {"MUL", 2, {ALL_REGS, ALL_REGS}, ALL_REGS, 0, F_COMMUTATIVE | F_ARG0},
    [OP_LESS] = {"LESS", 2, {ALL_REGS, ALL_REGS}, ALL_REGS, 0, 0},
    [OP_CALL] = {"CALL", 1, {R0, 0}, R0, ALL_REGS, F_CALL},
    [OP_PRINT] = {"PRINT", 1, {R0, 0}, 0, ALL_REGS, F_CALL | F_VOID},
    [OP_PRINTSTR] = {"PRINTSTR", 0, {0, 0}, 0, ALL_REGS, F_CALL | F_VOID},
    [OP_COPY] = {"COPY", 1, {0, 0}, ALL_REGS, 0, 0},
    [OP_STORE] = {"STORE", 1, {0, 0}, 0, 0, F_VOID},
    [OP_LOAD] = {"LOAD", 1, {0, 0}, ALL_REGS, 0, 0},
};

static const char *strings[] = {"Hello, world!"};

struct value {
    enum op op;
    int nargs;
    int args[2];
    long aux;
    int block; /* -1 for a spill that has no place yet */
};

enum kind { B_PLAIN, B_IF, B_RET };

struct block {
    enum kind kind;
    int ctrl; /* the condition of B_IF, the result of B_RET, or -1 */
    int nsuccs;
    int succs[2];
    int npreds;
    int preds[MAX_PREDS];
    int likely; /* 1 when the first successor of a B_IF is likely, else 0 */
    int nvals;
    int vals[MAX_BVALS];
};

struct func {
    const char *name;
    int nblocks;
    struct block blocks[MAX_BLOCKS];
    int nvals;
    struct value vals[MAX_VALUES];
    int home[MAX_VALUES]; /* a register, or NREGS + the number of a stack slot */
};

static struct func funcs[MAX_FUNCS];
static int nfuncs;

static int new_value(struct func *f, int block, enum op op, int a0)
{
    int id = f->nvals++;
    struct value *v = &f->vals[id];
    v->op = op;
    v->nargs = ops[op].nargs;
    v->args[0] = a0;
    v->args[1] = -1;
    v->aux = 0;
    v->block = block;
    f->home[id] = NO_HOME;
    if (block >= 0) {
        struct block *b = &f->blocks[block];
        b->vals[b->nvals++] = id;
    }
    return id;
}

/* ------------------------------------------------------------------------------------------
 * Building the examples
 * ---------------------------------------------------------------------------------------- */

static struct func *begin_func(const char *name)
{
    struct func *f = &funcs[nfuncs++];
    f->name = name;
    return f;
}

static int new_block(struct func *f, enum kind kind)
{
    struct block *b = &f->blocks[f->nblocks];
    b->kind = kind;
    b->ctrl = -1;
    return f->nblocks++;
}

static void edge(struct func *f, int from, int to)
{
    struct block *b = &f->blocks[from];
    struct block *t = &f->blocks[to];
    b->succs[b->nsuccs++] = to;
    t->preds[t->npreds++] = from;
}

static int val(struct func *f, int b, enum op op, long aux, int a0, int a1)
{
    int id = new_value(f, b, op, a0);
    f->vals[id].args[1] = a1;
    f->vals[id].aux = aux;
    return id;
}

static void build_hello(void)
{
    struct func *f = begin_func("main");
    int b0 = new_block(f, B_RET);
    val(f, b0, OP_PRINTSTR, 0, -1, -1);
}

static void build_arithmetic(void)
{
    struct func *f = begin_func("main");
    int b0 = new_block(f, B_RET);
    int c2 = val(f, b0, OP_CONST, 2, -1, -1);
    int c3 = val(f, b0, OP_CONST, 3, -1, -1);
    int sum = val(f, b0, OP_ADD, 0, c2, c3);
    int c4 = val(f, b0, OP_CONST, 4, -1, -1);
    int prod = val(f, b0, OP_MUL, 0, sum, c4);
    int c6 = val(f, b0, OP_CONST, 6, -1, -1);
    int diff = val(f, b0, OP_SUB, 0, prod, c6);
    val(f, b0, OP_PRINT, 0, diff, -1);
}

static void build_branch(void)
{
    struct func *f = begin_func("main");
    int b0 = new_block(f, B_IF);
    int b1 = new_block(f, B_PLAIN);
    int b2 = new_block(f, B_PLAIN);
    int b3 = new_block(f, B_RET);
    edge(f, b0, b1);
    edge(f, b0, b2);
    edge(f, b1, b3);
    edge(f, b2, b3);
    int a = val(f, b0, OP_CONST, 3, -1, -1);
    int b = val(f, b0, OP_CONST, 5, -1, -1);
    f->blocks[b0].ctrl = val(f, b0, OP_LESS, 0, a, b);
    val(f, b1, OP_PRINT, 0, b, -1);
    val(f, b2, OP_PRINT, 0, a, -1);
}

static void build_loop(void)
{
    struct func *f = begin_func("main");
    int b0 = new_block(f, B_PLAIN);
    int b1 = new_block(f, B_IF);
    int b2 = new_block(f, B_PLAIN);
    int b3 = new_block(f, B_RET);
    edge(f, b0, b1);
    edge(f, b1, b2);
    edge(f, b1, b3);
    edge(f, b2, b1);
    f->blocks[b1].likely = 1;
    int sum0 = val(f, b0, OP_CONST, 0, -1, -1);
    int one = val(f, b0, OP_CONST, 1, -1, -1);
    int i0 = val(f, b0, OP_ADD, 0, sum0, one);
    int sum, i;
    i = val(f, b1, OP_PHI, 0, i0, -1);
    sum = val(f, b1, OP_PHI, 0, sum0, -1);
    int c11 = val(f, b1, OP_CONST, 11, -1, -1);
    f->blocks[b1].ctrl = val(f, b1, OP_LESS, 0, i, c11);
    int sum2, i2, c1;
    sum2 = val(f, b2, OP_ADD, 0, i, sum);
    c1 = val(f, b2, OP_CONST, 1, -1, -1);
    i2 = val(f, b2, OP_ADD, 0, i, c1);
    f->vals[sum].args[1] = sum2;
    f->vals[i].args[1] = i2;
    val(f, b3, OP_PRINT, 0, sum, -1);
}

static void build_call(void)
{
    struct func *f = begin_func("fact");
    int b0 = new_block(f, B_IF);
    int b1 = new_block(f, B_RET);
    int b2 = new_block(f, B_RET);
    edge(f, b0, b1);
    edge(f, b0, b2);
    int n = val(f, b0, OP_ARG, 0, -1, -1);
    int c2 = val(f, b0, OP_CONST, 2, -1, -1);
    f->blocks[b0].ctrl = val(f, b0, OP_LESS, 0, n, c2);
    f->blocks[b1].ctrl = val(f, b1, OP_CONST, 1, -1, -1);
    int c1 = val(f, b2, OP_CONST, 1, -1, -1);
    int nm1 = val(f, b2, OP_SUB, 0, n, c1);
    int r = val(f, b2, OP_CALL, 0, nm1, -1);
    f->blocks[b2].ctrl = val(f, b2, OP_MUL, 0, n, r);

    f = begin_func("main");
    int m0 = new_block(f, B_RET);
    int c5 = val(f, m0, OP_CONST, 5, -1, -1);
    int res = val(f, m0, OP_CALL, 0, c5, -1);
    val(f, m0, OP_PRINT, 0, res, -1);
}

/* ------------------------------------------------------------------------------------------
 * Printing
 * ---------------------------------------------------------------------------------------- */

static void print_loc(int loc)
{
    if (loc < NREGS) {
        fprintf(stderr, "R%d", loc);
    } else {
        fprintf(stderr, "S%d", loc - NREGS);
    }
}

static void print_value(const struct func *f, int id)
{
    const struct value *v = &f->vals[id];
    fprintf(stderr, "    v%d = %s", id, ops[v->op].name);
    if (v->op == OP_CONST) {
        fprintf(stderr, " %ld", v->aux);
    }
    if (v->op == OP_CALL) {
        fprintf(stderr, " %s", funcs[v->aux].name);
    }
    if (v->op == OP_PRINTSTR) {
        fprintf(stderr, " \"%s\"", strings[v->aux]);
    }
    for (int i = 0; i < v->nargs; i++) {
        fprintf(stderr, "%s v%d", i == 0 ? "" : ",", v->args[i]);
    }
    if (f->home[id] != NO_HOME) {
        fprintf(stderr, "  : ");
        print_loc(f->home[id]);
    }
    fprintf(stderr, "\n");
}

static void print_func(const struct func *f)
{
    fprintf(stderr, "func %s:\n", f->name);
    for (int bi = 0; bi < f->nblocks; bi++) {
        const struct block *b = &f->blocks[bi];
        fprintf(stderr, "  b%d:", bi);
        for (int i = 0; i < b->npreds; i++) {
            fprintf(stderr, "%s b%d", i == 0 ? " (from" : ",", b->preds[i]);
            if (i == b->npreds - 1) {
                fprintf(stderr, ")");
            }
        }
        fprintf(stderr, "\n");
        for (int i = 0; i < b->nvals; i++) {
            print_value(f, b->vals[i]);
        }
        static const char *const ctrl_fmt[] = {"    goto b%d\n", "    if v%d goto b%d else b%d\n",
                                               "    return v%d\n"};
        if (b->kind == B_PLAIN) {
            fprintf(stderr, ctrl_fmt[0], b->succs[0]);
        } else if (b->kind == B_IF) {
            fprintf(stderr, ctrl_fmt[1], b->ctrl, b->succs[0], b->succs[1]);
        } else if (b->ctrl >= 0) {
            fprintf(stderr, ctrl_fmt[2], b->ctrl);
        } else {
            fprintf(stderr, "    return\n");
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * The allocator's state
 * ---------------------------------------------------------------------------------------- */

struct endreg {
    int r;
    int v; /* the value from the SSA that the register holds */
    int c; /* the copy of it that is in the register */
};

struct vinfo {
    regmask regs;   /* the registers that hold a copy of the value */
    bool needreg;
    bool remat;
    int uses[MAX_USES]; /* distances of the uses in this block, the nearest one last */
    int nuses;
    int spill;      /* the STORE that saves the value, or -1 */
    int restore_min; /* the part of the dominator tree that contains restores */
    int restore_max;
};

struct ra {
    struct func *f;
    int orig[MAX_VALUES]; /* the value of the SSA that a copy, a load or a store stands for */
    struct vinfo values[MAX_VALUES];

    int live[MAX_BLOCKS][MAX_VALUES]; /* distance to the next use at the end of a block, or -1 */

    unsigned dom[MAX_BLOCKS]; /* the blocks that dominate a block, as a set */
    int idom[MAX_BLOCKS];
    int entry[MAX_BLOCKS];    /* the numbering of the dominator tree that tells subtrees apart */
    int exit[MAX_BLOCKS];
    int depth[MAX_BLOCKS];    /* the number of loops around a block */

    struct endreg end_regs[MAX_BLOCKS][MAX_LOCS];
    int n_end[MAX_BLOCKS];
    int start_copy[MAX_BLOCKS][MAX_VALUES]; /* the copy of a value in a register at the start, or -1 */

    struct {
        int v;
        int c;
    } regs[NREGS];
    regmask used;
    regmask nospill; /* the registers of the arguments of the instruction being allocated */
    int next_call[MAX_BVALS + 1];
    int cur_idx;
    int cur_block;
};

static struct ra ras[MAX_FUNCS];

/* ------------------------------------------------------------------------------------------
 * Dominators and loops
 * ---------------------------------------------------------------------------------------- */

static int number_tree(struct ra *s, int b, int counter)
{
    s->entry[b] = counter++;
    for (int c = 0; c < s->f->nblocks; c++) {
        if (c != 0 && s->idom[c] == b) {
            counter = number_tree(s, c, counter);
        }
    }
    s->exit[b] = counter++;
    return counter;
}

static void compute_doms(struct ra *s)
{
    const struct func *f = s->f;
    int n = f->nblocks;
    for (int b = 0; b < n; b++) {
        s->dom[b] = b == 0 ? 1u : (1u << n) - 1;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (int b = 1; b < n; b++) {
            unsigned d = (1u << n) - 1;
            for (int i = 0; i < f->blocks[b].npreds; i++) {
                d &= s->dom[f->blocks[b].preds[i]];
            }
            d |= BIT(b);
            changed |= d != s->dom[b];
            s->dom[b] = d;
        }
    }
    /* The immediate dominator is the strict dominator that all the others dominate. */
    for (int b = 1; b < n; b++) {
        for (int d = 0; d < n; d++) {
            if (d != b && (s->dom[b] >> d & 1) && s->dom[d] == (s->dom[b] & ~BIT(b))) {
                s->idom[b] = d;
            }
        }
    }
    number_tree(s, 0, 0);
    /* A back edge goes to a block that dominates its source. The loop is everything that
     * reaches the source without passing the header. */
    for (int p = 0; p < n; p++) {
        for (int i = 0; i < f->blocks[p].nsuccs; i++) {
            int h = f->blocks[p].succs[i];
            if (!(s->dom[p] >> h & 1)) {
                continue;
            }
            unsigned body = BIT(h) | BIT(p);
            for (bool grew = true; grew;) {
                grew = false;
                for (int b = 0; b < n; b++) {
                    if (!(body >> b & 1) || b == h) {
                        continue;
                    }
                    for (int k = 0; k < f->blocks[b].npreds; k++) {
                        unsigned before = body;
                        body |= BIT(f->blocks[b].preds[k]);
                        grew |= body != before;
                    }
                }
            }
            for (int b = 0; b < n; b++) {
                s->depth[b] += body >> b & 1;
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Liveness: how far away is the next use
 * ---------------------------------------------------------------------------------------- */

static void compute_live(struct ra *s)
{
    struct func *f = s->f;
    memset(s->live, -1, sizeof(s->live));
    for (bool changed = true; changed;) {
        changed = false;
        for (int bi = f->nblocks - 1; bi >= 0; bi--) {
            const struct block *b = &f->blocks[bi];
            /* The distance to the next use from the start of the block. */
            int d[MAX_VALUES];
            for (int v = 0; v < f->nvals; v++) {
                d[v] = s->live[bi][v] < 0 ? -1 : s->live[bi][v] + b->nvals;
            }
            if (b->ctrl >= 0) {
                d[b->ctrl] = b->nvals;
            }
            int phis[MAX_BVALS];
            int nphis = 0;
            for (int i = b->nvals - 1; i >= 0; i--) {
                const struct value *v = &f->vals[b->vals[i]];
                d[b->vals[i]] = -1;
                if (v->op == OP_PHI) {
                    phis[nphis++] = b->vals[i];
                    continue;
                }
                if (ops[v->op].flags & F_CALL) {
                    for (int x = 0; x < f->nvals; x++) {
                        d[x] += d[x] >= 0 ? UNLIKELY_DISTANCE : 0;
                    }
                }
                for (int k = 0; k < v->nargs; k++) {
                    d[v->args[k]] = i;
                }
            }
            for (int k = 0; k < b->npreds; k++) {
                int p = b->preds[k];
                const struct block *pb = &f->blocks[p];
                int delta = NORMAL_DISTANCE;
                if (pb->kind == B_IF && pb->likely) {
                    delta = pb->succs[0] == bi ? LIKELY_DISTANCE : UNLIKELY_DISTANCE;
                }
                for (int x = 0; x < f->nvals; x++) {
                    if (d[x] >= 0 && (s->live[p][x] < 0 || d[x] + delta < s->live[p][x])) {
                        s->live[p][x] = d[x] + delta;
                        changed = true;
                    }
                }
                for (int i = 0; i < nphis; i++) {
                    int id = f->vals[phis[i]].args[k];
                    if (s->live[p][id] < 0) {
                        s->live[p][id] = delta;
                        changed = true;
                    }
                }
            }
        }
    }
}

static void init_ra(struct ra *s, struct func *f)
{
    memset(s, 0, sizeof(*s));
    s->f = f;
    for (int v = 0; v < f->nvals; v++) {
        s->orig[v] = v;
        s->values[v].needreg = !(ops[f->vals[v].op].flags & F_VOID);
        s->values[v].remat = ops[f->vals[v].op].flags & F_REMAT;
        s->values[v].spill = -1;
    }
    for (int r = 0; r < NREGS; r++) {
        s->regs[r].v = -1;
    }
    memset(s->start_copy, -1, sizeof(s->start_copy));
    compute_doms(s);
    compute_live(s);
}

static void print_live(const struct ra *s)
{
    const struct func *f = s->f;
    fprintf(stderr, "func %s: loop depth and the values live at the end of each block, with the "
                    "distance to their next use\n",
            f->name);
    for (int bi = 0; bi < f->nblocks; bi++) {
        fprintf(stderr, "  b%d (loop depth %d):", bi, s->depth[bi]);
        for (int v = 0; v < f->nvals; v++) {
            if (s->live[bi][v] >= 0) {
                fprintf(stderr, " v%d(%d)", v, s->live[bi][v]);
            }
        }
        fprintf(stderr, "\n");
    }
}

/* ------------------------------------------------------------------------------------------
 * The greedy walk
 * ---------------------------------------------------------------------------------------- */

static void add_use(struct ra *s, int v, int dist)
{
    struct vinfo *vi = &s->values[v];
    vi->uses[vi->nuses++] = dist;
}

/* The distance of the nearest use of v in the current block. */
static int next_use(const struct ra *s, int v)
{
    return s->values[v].uses[s->values[v].nuses - 1];
}

static void append_val(struct func *f, int block, int id)
{
    struct block *b = &f->blocks[block];
    b->vals[b->nvals++] = id;
    f->vals[id].block = block;
}

static void free_reg(struct ra *s, int r)
{
    int v = s->regs[r].v;
    s->regs[r].v = -1;
    s->values[v].regs &= ~BIT(r);
    s->used &= ~BIT(r);
}

static void free_regs(struct ra *s, regmask m)
{
    while (m & s->used) {
        free_reg(s, lowest(m & s->used));
    }
}

/* Register r now holds c, a copy of v. */
static void assign_reg(struct ra *s, int r, int v, int c)
{
    s->regs[r].v = v;
    s->regs[r].c = c;
    s->values[v].regs |= BIT(r);
    s->used |= BIT(r);
    s->f->home[c] = r;
}

static void set_orig(struct ra *s, int c, int v)
{
    s->orig[c] = s->orig[v];
}

/* Choose a register from mask. If all are taken, the one whose value is used farthest in the
 * future is kicked out. */
static int alloc_reg(struct ra *s, regmask mask)
{
    mask &= ~s->nospill;
    if (mask & ~s->used) {
        return lowest(mask & ~s->used);
    }
    int r = -1;
    int maxuse = -1;
    for (int t = 0; t < NREGS; t++) {
        if ((mask >> t & 1) && next_use(s, s->regs[t].v) > maxuse) {
            r = t;
            maxuse = next_use(s, s->regs[t].v);
        }
    }
    free_reg(s, r);
    return r;
}

/* The spill of v. Its place is not known yet, so it has no block. It only records which part of
 * the dominator tree b is in, because the spill must dominate all restores. */
static int make_spill(struct ra *s, int v, int b)
{
    struct vinfo *vi = &s->values[v];
    if (vi->spill >= 0) {
        vi->restore_min = s->entry[b] < vi->restore_min ? s->entry[b] : vi->restore_min;
        vi->restore_max = s->exit[b] > vi->restore_max ? s->exit[b] : vi->restore_max;
        return vi->spill;
    }
    vi->spill = new_value(s->f, -1, OP_STORE, -1);
    set_orig(s, vi->spill, v);
    vi->restore_min = s->entry[b];
    vi->restore_max = s->exit[b];
    return vi->spill;
}

/* Put v in a register from mask and return the value that is in it: v, or a copy of v made by
 * copying it from another register, computing it again or loading it from its spill. */
static int alloc_val_to_reg(struct ra *s, int v, regmask mask, bool nospill)
{
    struct vinfo *vi = &s->values[v];
    if (mask & vi->regs) {
        int r = lowest(mask & vi->regs);
        s->nospill |= nospill ? BIT(r) : 0;
        return s->regs[r].c;
    }
    int r = alloc_reg(s, mask);
    int c;
    if (vi->regs) {
        c = new_value(s->f, s->cur_block, OP_COPY, s->regs[lowest(vi->regs)].c);
    } else if (vi->remat) {
        c = val(s->f, s->cur_block, s->f->vals[v].op, s->f->vals[v].aux, -1, -1);
    } else {
        c = new_value(s->f, s->cur_block, OP_LOAD, make_spill(s, v, s->cur_block));
    }
    set_orig(s, c, v);
    assign_reg(s, r, v, c);
    s->nospill |= nospill ? BIT(r) : 0;
    return c;
}

/* The instruction v has used its arguments. Free the registers of those that are dead, or not
 * used again before the next call, which would kick them out anyway. */
static void advance_uses(struct ra *s, int v)
{
    const struct value *val = &s->f->vals[v];
    for (int i = 0; i < val->nargs; i++) {
        struct vinfo *ai = &s->values[val->args[i]];
        ai->nuses--;
        if (ai->nuses == 0 || next_use(s, val->args[i]) > s->next_call[s->cur_idx]) {
            free_regs(s, ai->regs);
        }
    }
}

static void set_state(struct ra *s, const struct endreg *regs, int n)
{
    free_regs(s, s->used);
    for (int i = 0; i < n; i++) {
        assign_reg(s, regs[i].r, regs[i].v, regs[i].c);
    }
}

/* Allocate one instruction: its arguments go in registers, then its result. */
static void alloc_value(struct ra *s, int v)
{
    struct func *f = s->f;
    struct value *val = &f->vals[v];
    const struct opinfo *oi = &ops[val->op];
    if (s->values[v].remat) {
        return; /* issued again before each use, see alloc_val_to_reg */
    }
    int args[2] = {-1, -1};
    /* An argument that must be in one register and already is stays. */
    for (int i = 0; i < val->nargs; i++) {
        if (count_regs(oi->in[i]) == 1 && (oi->in[i] & s->values[val->args[i]].regs)) {
            args[i] = alloc_val_to_reg(s, val->args[i], oi->in[i], true);
        }
    }
    for (int i = 0; i < val->nargs; i++) {
        if (args[i] < 0) {
            args[i] = alloc_val_to_reg(s, val->args[i], oi->in[i], true);
        }
    }
    s->nospill = 0;
    advance_uses(s, v);
    free_regs(s, oi->clobbers);
    if (oi->out) {
        regmask mask = (oi->flags & F_ARG0) ? BIT(f->home[args[0]]) : oi->out;
        assign_reg(s, alloc_reg(s, mask), v, v);
    }
    for (int i = 0; i < val->nargs; i++) {
        val->args[i] = args[i];
    }
    append_val(f, s->cur_block, v);
}

/* The state of the registers at the start of block bi. The tricky case is a block with several
 * predecessors: it starts from one of them, and every other predecessor has to be made to
 * agree later by `shuffle`. The phis are placed in registers if the inputs of that predecessor
 * are in registers. */
static void start_block(struct ra *s, int bi, const int *phis, int nphi, const bool *live_set)
{
    struct func *f = s->f;
    struct block *b = &f->blocks[bi];
    if (b->npreds == 1) {
        set_state(s, s->end_regs[b->preds[0]], s->n_end[b->preds[0]]);
        for (int r = 0; r < NREGS; r++) {
            if (s->regs[r].v >= 0 && !live_set[s->regs[r].v]) {
                free_reg(s, r);
            }
        }
        return;
    }
    if (b->npreds == 0) {
        return;
    }
    /* Start from the first predecessor, which has been walked already. */
    int idx = 0; /* the blocks are in order, so the first predecessor was walked */
    int p = b->preds[idx];
    set_state(s, s->end_regs[p], s->n_end[p]);

    /* A phi gets the register of its input in that predecessor. */
    int phi_reg[MAX_BVALS];
    regmask phi_used = 0;
    for (int i = 0; i < nphi; i++) {
        regmask m = s->values[f->vals[phis[i]].args[idx]].regs & ~phi_used;
        phi_reg[i] = m ? lowest(m) : NO_REG;
        phi_used |= m ? BIT(phi_reg[i]) : 0;
    }
    /* Free those registers. The real allocator first copies an input that other things still
     * use to a free register; see FIDELITY.md. */
    for (int i = 0; i < nphi; i++) {
        if (phi_reg[i] != NO_REG) {
            free_reg(s, phi_reg[i]);
        }
    }
    /* A phi whose input was not in a register takes a free one, or lives on the stack. */
    for (int i = 0; i < nphi; i++) {
        regmask m = ALL_REGS & ~phi_used & ~s->used;
        if (phi_reg[i] == NO_REG) {
            phi_reg[i] = lowest(m);
            phi_used |= BIT(phi_reg[i]);
        }
    }
    for (int i = 0; i < nphi; i++) {
        append_val(f, bi, phis[i]);
        assign_reg(s, phi_reg[i], phis[i], phis[i]);
    }
}

static void alloc_block(struct ra *s, int bi)
{
    struct func *f = s->f;
    struct block *b = &f->blocks[bi];
    s->cur_block = bi;

    /* The distance to the next use of every value, walking the block backwards, and the next
     * call after every instruction. */
    bool live_set[MAX_VALUES] = {false};
    for (int v = 0; v < f->nvals; v++) {
        if (s->live[bi][v] >= 0) {
            add_use(s, v, b->nvals + s->live[bi][v]);
            live_set[v] = true;
        }
    }
    if (b->ctrl >= 0) {
        add_use(s, b->ctrl, b->nvals);
        live_set[b->ctrl] = true;
    }
    int next_call = INT_MAX;
    s->next_call[b->nvals] = INT_MAX;
    for (int i = b->nvals - 1; i >= 0; i--) {
        const struct value *v = &f->vals[b->vals[i]];
        live_set[b->vals[i]] = false;
        if (v->op != OP_PHI) {
            if (ops[v->op].flags & F_CALL) {
                memset(live_set, 0, sizeof(live_set));
                next_call = i;
            }
            for (int k = 0; k < v->nargs; k++) {
                add_use(s, v->args[k], i);
                live_set[v->args[k]] = true;
            }
        }
        s->next_call[i] = next_call;
    }

    int phis[MAX_BVALS];
    int old[MAX_BVALS];
    int nphi = 0;
    int nold = 0;
    for (int i = 0; i < b->nvals; i++) {
        if (f->vals[b->vals[i]].op == OP_PHI) {
            phis[nphi++] = b->vals[i];
        } else {
            old[nold++] = b->vals[i];
        }
    }
    b->nvals = 0;
    start_block(s, bi, phis, nphi, live_set);
    for (int r = 0; r < NREGS; r++) {
        if (s->regs[r].v >= 0) {
            s->start_copy[bi][s->regs[r].v] = s->regs[r].c;
        }
    }
    for (int i = 0; i < nold; i++) {
        s->cur_idx = nphi + i;
        alloc_value(s, old[i]);
    }

    /* The control value goes in any register. */
    if (b->ctrl >= 0) {
        int v = b->ctrl;
        b->ctrl = alloc_val_to_reg(s, v, ALL_REGS, false);
        s->values[v].nuses--;
        free_regs(s, s->values[v].regs); /* a control value is not used again */
    }

    for (int r = 0; r < NREGS; r++) {
        if (s->regs[r].v >= 0) {
            s->end_regs[bi][s->n_end[bi]++] = (struct endreg){r, s->regs[r].v, s->regs[r].c};
        }
    }
    /* A value that is live at the end but in no register will be restored later, so it needs a
     * spill. */
    for (int v = 0; v < f->nvals; v++) {
        if (s->live[bi][v] >= 0 && !s->values[v].regs && !s->values[v].remat) {
            make_spill(s, v, bi);
        }
        s->values[v].nuses = 0;
    }
}

/* ------------------------------------------------------------------------------------------
 * Placing the spills
 * ---------------------------------------------------------------------------------------- */

static bool must_be_first(enum op op)
{
    return op == OP_PHI || op == OP_ARG;
}

/* Each spill goes in the highest block that dominates all restores and has the value in a
 * register at its start, and never in a deeper loop than the definition. That is often a long
 * way below the definition: a value that is only restored on one path is only saved on it. */
static void place_spills(struct ra *s)
{
    struct func *f = s->f;
    int start[MAX_BLOCKS][MAX_VALUES];
    int nstart[MAX_BLOCKS] = {0};
    int after[MAX_VALUES];
    memset(after, -1, sizeof(after));
    int nvals = f->nvals;
    for (int v = 0; v < nvals; v++) {
        const struct vinfo *vi = &s->values[v];
        if (vi->spill < 0 || f->vals[vi->spill].block >= 0) {
            continue; /* no spill */
        }
        int best = f->vals[v].block;
        int best_arg = v;
        int b = best;
        for (;;) {
            int p = b;
            b = -1;
            for (int c = 1; c < f->nblocks; c++) {
                if (s->idom[c] == p && s->entry[c] <= vi->restore_min &&
                    s->exit[c] >= vi->restore_max) {
                    b = c;
                    break;
                }
            }
            if (b < 0) {
                break;
            }
            /* Go down into b if the value is in a register there; otherwise stay (picked by
             * index, there is nothing to decide that a branch would make clearer). */
            int c = s->start_copy[b][v];
            int down = c >= 0;
            best = (int[]){best, b}[down];
            best_arg = (int[]){best_arg, c}[down];
        }
        f->vals[vi->spill].block = best;
        f->vals[vi->spill].args[0] = best_arg;
        if (best == f->vals[v].block && !must_be_first(f->vals[v].op)) {
            after[v] = vi->spill;
        } else {
            start[best][nstart[best]++] = vi->spill;
        }
    }
    for (int bi = 0; bi < f->nblocks; bi++) {
        struct block *b = &f->blocks[bi];
        int old[MAX_BVALS];
        int nold = b->nvals;
        memcpy(old, b->vals, sizeof(int) * (size_t)nold);
        b->nvals = 0;
        int i = 0;
        for (; i < nold && must_be_first(f->vals[old[i]].op); i++) {
            b->vals[b->nvals++] = old[i];
        }
        for (int k = 0; k < nstart[bi]; k++) {
            b->vals[b->nvals++] = start[bi][k];
        }
        for (; i < nold; i++) {
            b->vals[b->nvals++] = old[i];
            if (after[old[i]] >= 0) {
                b->vals[b->nvals++] = after[old[i]];
            }
        }
    }
}

/* Everything that did not get a register gets a stack slot: the spills. The
 * real stackalloc lets slots that are not live together share, here every one has its own. */
static void stack_alloc(struct ra *s)
{
    struct func *f = s->f;
    int nslots = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        for (int i = 0; i < f->blocks[bi].nvals; i++) {
            int v = f->blocks[bi].vals[i];
            if (f->vals[v].op == OP_STORE) {
                f->home[v] = NREGS + nslots++;
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Fixing up the merge edges
 * ---------------------------------------------------------------------------------------- */

struct content {
    int vid; /* the value of the SSA */
    int c;   /* the copy of it that is there */
};

struct dest {
    int loc;
    int vid;
    int *splice; /* the phi argument that must name what ends up there */
};

/* The moves on the edge from p to b. The sources are what p leaves in registers and in stack
 * slots, the destinations are the registers of the phis of b. */
struct edge_state {
    struct ra *s;
    int p;
    int b;
    int cache[MAX_VALUES][MAX_LOCS]; /* for each value, the copies of it that exist now */
    int ncache[MAX_VALUES];
    struct content contents[MAX_LOCS];
    struct dest dests[MAX_DESTS];
    int ndests;
};

static void es_set(struct edge_state *e, int loc, int vid, int c)
{
    e->s->f->home[c] = loc;
    e->contents[loc] = (struct content){vid, c};
    e->cache[vid][e->ncache[vid]++] = c;
}

/* Put value vid in location loc: a copy of a source, a register preferred, or the value
 * computed again, or loaded from its stack slot. */
static void process_dest(struct edge_state *e, int loc, int vid, int *splice)
{
    struct ra *s = e->s;
    struct func *f = s->f;
    struct content occ = e->contents[loc];
    if (occ.vid == vid) {
        *splice = occ.c;
        return;
    }
    int c = e->cache[vid][0]; /* the real one prefers a register when there are several */
    int x;
    if (s->values[vid].remat) {
        const struct value *v = &f->vals[vid];
        x = val(f, e->p, v->op, v->aux, -1, -1);
    } else if (f->home[c] < NREGS) {
        x = new_value(f, e->p, OP_COPY, c);
    } else {
        x = new_value(f, e->p, OP_LOAD, c);
    }
    set_orig(s, x, vid);
    es_set(e, loc, vid, x);
    *splice = x;
}

static void es_setup(struct edge_state *e, int idx)
{
    struct ra *s = e->s;
    struct func *f = s->f;
    const struct block *b = &f->blocks[e->b];
    memset(e->ncache, 0, sizeof(e->ncache));
    for (int loc = 0; loc < MAX_LOCS; loc++) {
        e->contents[loc] = (struct content){-1, -1};
    }
    e->ndests = 0;
    for (int i = 0; i < s->n_end[e->p]; i++) {
        const struct endreg *x = &s->end_regs[e->p][i];
        es_set(e, x->r, x->v, x->c);
    }
    /* A value that is live at the end of p and has a spill can be loaded from its stack slot. */
    for (int v = 0; v < f->nvals; v++) {
        int spill = s->values[v].spill;
        if (s->live[e->p][v] >= 0 && spill >= 0) {
            es_set(e, f->home[spill], v, spill);
        }
    }
    for (int i = 0; i < b->nvals && f->vals[b->vals[i]].op == OP_PHI; i++) {
        struct value *phi = &f->vals[b->vals[i]];
        e->dests[e->ndests++] = (struct dest){f->home[b->vals[i]], phi->args[idx], &phi->args[idx]};
    }
}

static void shuffle(struct ra *s)
{
    static struct edge_state e;
    e.s = s;
    for (int bi = 0; bi < s->f->nblocks; bi++) {
        const struct block *b = &s->f->blocks[bi];
        for (int i = 0; b->npreds > 1 && i < b->npreds; i++) {
            e.b = bi;
            e.p = b->preds[i];
            es_setup(&e, i);
            for (int k = 0; k < e.ndests; k++) {
                process_dest(&e, e.dests[k].loc, e.dests[k].vid, e.dests[k].splice);
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Running the allocated code
 * ---------------------------------------------------------------------------------------- */

static long machine[NREGS];

static long run_func(int fi, long arg, int depth)
{
    const struct func *f = &funcs[fi];
    long slots[MAX_SLOTS];
    machine[0] = arg;
    int bi = 0;
    for (;;) {
        const struct block *b = &f->blocks[bi];
        for (int i = 0; i < b->nvals; i++) {
            const struct value *v = &f->vals[b->vals[i]];
            long scratch; /* where the result of an instruction without a register goes */
            int home = f->home[b->vals[i]];
            long *dst = home >= 0 && home < NREGS ? &machine[home] : &scratch;
            long a = v->nargs > 0 && f->home[v->args[0]] < NREGS ? machine[f->home[v->args[0]]] : 0;
            long c = v->nargs > 1 ? machine[f->home[v->args[1]]] : 0;
            switch (v->op) {
            case OP_CONST:
                *dst = v->aux;
                break;
            case OP_ADD:
                *dst = a + c;
                break;
            case OP_SUB:
                *dst = a - c;
                break;
            case OP_MUL:
                *dst = a * c;
                break;
            case OP_LESS:
                *dst = a < c;
                break;
            case OP_COPY:
                *dst = a;
                break;
            case OP_STORE:
                slots[home - NREGS] = a;
                break;
            case OP_LOAD:
                *dst = slots[f->home[v->args[0]] - NREGS];
                break;
            case OP_CALL: {
                long r = run_func((int)v->aux, a, depth + 1);
                for (int k = 0; k < NREGS; k++) {
                    machine[k] = POISON;
                }
                *dst = r;
                break;
            }
            case OP_PRINT:
            case OP_PRINTSTR:
                if (v->op == OP_PRINT) {
                    printf("%ld\n", a);
                } else {
                    printf("%s\n", strings[v->aux]);
                }
                for (int k = 0; k < NREGS; k++) {
                    machine[k] = POISON;
                }
                break;
            default: /* OP_ARG and OP_PHI */
                break; /* the argument is in R0 on entry, and the moves of a phi are on its edges */
            }
        }
        if (b->kind == B_RET) {
            return b->ctrl >= 0 ? machine[f->home[b->ctrl]] : 0;
        }
        bi = b->kind == B_PLAIN || machine[f->home[b->ctrl]] ? b->succs[0] : b->succs[1];
    }
}

/* ------------------------------------------------------------------------------------------
 * The stages
 * ---------------------------------------------------------------------------------------- */

struct example {
    const char *name;
    const char *source;
    void (*build)(void);
};

static const struct example examples[] = {
    {"arithmetic", "print((2 + 3) * 4 - 6)", build_arithmetic},
    {"branch", "local a = 3\nlocal b = 5\nif a < b then\n  print(b)\nelse\n  print(a)\nend",
     build_branch},
    {"call",
     "local function fact(n)\n  if n < 2 then\n    return 1\n  end\n  return n * fact(n - 1)\nend\n"
     "print(fact(5))",
     build_call},
    {"hello", "print(\"Hello, world!\")", build_hello},
    {"loop",
     "local sum = 0\nlocal i = 1\nwhile i < 11 do\n  sum = sum + i\n  i = i + 1\nend\nprint(sum)",
     build_loop},
};

static const struct example *stage_source(const char *name)
{
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        if (strcmp(examples[i].name, name) == 0) {
            fprintf(stderr, "== stage_source: Lua source of %s ==\n%s\n", name,
                    examples[i].source);
            return &examples[i];
        }
    }
    return NULL;
}

static void stage_build(const struct example *example)
{
    example->build();
    fprintf(stderr, "== stage_build: the SSA of each function, %d registers ==\n", NREGS);
    for (int i = 0; i < nfuncs; i++) {
        print_func(&funcs[i]);
    }
}

static void stage_live(void)
{
    fprintf(stderr, "== stage_live: the next use of each value at the end of each block ==\n");
    for (int i = 0; i < nfuncs; i++) {
        init_ra(&ras[i], &funcs[i]);
        print_live(&ras[i]);
    }
}

static void print_end_regs(const struct ra *s)
{
    for (int bi = 0; bi < s->f->nblocks; bi++) {
        fprintf(stderr, "  registers at the end of b%d:", bi);
        for (int i = 0; i < s->n_end[bi]; i++) {
            fprintf(stderr, " R%d=v%d", s->end_regs[bi][i].r, s->end_regs[bi][i].v);
        }
        fprintf(stderr, "\n");
    }
}

static void stage_regalloc(void)
{
    fprintf(stderr, "== stage_regalloc: the greedy walk, the spills are not placed yet ==\n");
    for (int i = 0; i < nfuncs; i++) {
        for (int bi = 0; bi < funcs[i].nblocks; bi++) {
            alloc_block(&ras[i], bi);
        }
        print_func(&funcs[i]);
        print_end_regs(&ras[i]);
    }
}

static void stage_spills(void)
{
    fprintf(stderr, "== stage_spills: where each spill goes, and the stack slots ==\n");
    for (int i = 0; i < nfuncs; i++) {
        place_spills(&ras[i]);
        stack_alloc(&ras[i]);
        print_func(&funcs[i]);
    }
}

static void stage_shuffle(void)
{
    fprintf(stderr, "== stage_shuffle: the moves on the merge edges ==\n");
    for (int i = 0; i < nfuncs; i++) {
        shuffle(&ras[i]);
        print_func(&funcs[i]);
    }
}

static void stage_run(void)
{
    fprintf(stderr, "== stage_run: the output of the program ==\n");
    run_func(nfuncs - 1, 0, 0);
}

int main(int argc, char **argv)
{
    const struct example *example = NULL;
    if (argc < 2 || (example = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_build(example);
    stage_live();
    stage_regalloc();
    stage_spills();
    stage_shuffle();
    stage_run();
    return 0;
}
