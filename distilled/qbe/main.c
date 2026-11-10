/*
 * qbe: a cut-down version of the QBE compiler backend for x86-64 (System V).
 *
 * QBE takes a small SSA-based IL in which a frontend may assign to the same temporary more
 * than once, so the first thing it does is build proper SSA: a PHI goes into the dominance
 * frontier of the blocks that define a temporary, but only where the temporary is live. The
 * ABI pass then turns parameters, call arguments and return values into copies to and from
 * physical registers, and instruction selection (a backward walk over each block) fuses a
 * comparison into the jump that uses it. The register allocator works block by block, from
 * the innermost loop outwards, walking each block backwards: a use allocates a register, a
 * definition frees it, hints pull a temporary towards the register of the copy it feeds, and
 * every PHI and every disagreement between two blocks becomes a parallel move, placed on the
 * edge in a new block if it has to be.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout, the form after each stage on stderr, and writes
 *   the machine code to code.bin if a path is given.
 *
 * Stages, in the order of the real pipeline:
 *   stage_il      the IL of each function, as a frontend would write it
 *   stage_ssa     dominators, dominance frontiers, liveness, PHI insertion, renaming
 *   stage_abi     parameters, calls and returns become copies to and from registers
 *   stage_isel    amd64 instruction selection, backwards, with compare-and-branch fusion
 *   stage_live    liveness again, loop depth and spill costs
 *   stage_spill   what the spill pass tells the allocator about registers live across a call
 *   stage_rega    register allocation: hints, parallel moves, edge blocks
 *   stage_emit    jump threading, frame layout, assembly text and x86-64 bytes
 *   stage_export  write code.bin, link the runtime and run main
 */
#define _DEFAULT_SOURCE
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

#define MAXB 12
#define MAXI 40
#define MAXT 60
#define MAXP 4
#define NINS 256
#define HELLO_STRING "Hello, world!\n"

typedef uint64_t bits;
#define BIT(n) ((bits)1 << (n))

/* ------------------------------------------------------------------------------------------
 * Registers, references, instructions, blocks. Temporaries below TMP0 are the physical
 * registers, as in QBE, so a register can appear wherever a temporary can. A set of
 * temporaries is a 64-bit mask.
 * ---------------------------------------------------------------------------------------- */

enum { RAX = 1, RCX, RDX, RSI, RDI, R8, R9, R10, R11, RBX, R12, R13, R14, R15, TMP0 = 16 };
#define REGS (BIT(TMP0) - 2)
static const char *const reg64[] = {"",    "rax", "rcx", "rdx", "rsi", "rdi", "r8",  "r9",
                                    "r10", "r11", "rbx", "r12", "r13", "r14", "r15"};
static const char *const reg32[] = {"",     "eax",  "ecx",  "edx",  "esi",  "edi",
                                    "r8d",  "r9d",  "r10d", "r11d", "ebx",  "r12d",
                                    "r13d", "r14d", "r15d"};
/* the number of each register in x86 encodings */
static const int hwreg[] = {0, 0, 1, 2, 6, 7, 8, 9, 10, 11, 3, 12, 13, 14, 15};
/* caller-save registers, with the argument registers first, and callee-save registers */
static const int rsave[] = {RDI, RSI, RDX, RCX, R8, R9, R10, R11, RAX, -1};
static const int rclob[] = {RBX, R12, R13, R14, R15, -1};

enum { Kw, Kl };
enum { K_NONE, K_TMP, K_INT, K_SYM, K_CALL };
struct ref {
    int k, v;
};
static const struct ref NOREF = {K_NONE, 0};
static struct ref tmpr(int t) { return (struct ref){K_TMP, t}; }
static struct ref intr(int n) { return (struct ref){K_INT, n}; }
static struct ref symr(int s) { return (struct ref){K_SYM, s}; }
static struct ref callr(int ca) { return (struct ref){K_CALL, ca}; }
static int req(struct ref a, struct ref b) { return (a.k == b.k) & (a.v == b.v); }
static int isreg(struct ref r) { return (r.k == K_TMP) & (r.v < TMP0); }
static int istmp(struct ref r) { return r.k == K_TMP; }

/* a call is described by CALL(ca): ca >> 4 argument registers, ca & 1 for a result in rax */
static bits retregs(struct ref r) { return (r.v & 1) ? BIT(RAX) : 0; }
static bits argregs(struct ref r)
{
    bits b = 0;
    for (int j = 0; j < (r.v >> 4); j++) {
        b |= BIT(rsave[j]);
    }
    return b;
}

enum { S_FACT, S_MAIN, S_PRINT_INT, S_PRINT_STR, S_STR };
static const char *const symname[] = {"fact", "main", "print_int", "print_str", "str"};

enum { Onop, Opar, Ocopy, Oadd, Osub, Omul, Ocsltw, Oarg, Ocall, Oaddr, Oxcmp };
static const char *const opname[] = {"nop", "par", "copy", "add",  "sub", "mul",
                                     "csltw", "arg", "call", "addr", "xcmp"};

struct ins {
    int op, cls;
    struct ref to, arg[2];
};

struct phi {
    struct ref to;
    int cls, narg;
    struct ref arg[MAXP];
    struct blk *blk[MAXP];
};

enum { Jret0, Jretw, Jjmp, Jjnz, Jjlt };
struct blk {
    char name[32];
    struct phi phi[3];
    int nphi;
    struct ins ins[MAXI];
    int nins;
    struct {
        int type;
        struct ref arg;
    } jmp;
    struct blk *s1, *s2;
    struct blk *pred[MAXP];
    int npred;
    int id, loop, visit;
    struct blk *idom, *dom, *dlink;
    struct blk *fron[MAXB];
    int nfron;
    bits in, out, gen;
};

struct tmp {
    char name[48], base[16];
    int cls;
    int ndef, nuse, bid, phi, visit;
    bits usebm, avoid; /* avoid: registers this temporary must not get, QBE's hint.m */
    int hint_r, hint_w, last;
    int cost;
};

struct fn {
    int sym;
    struct blk blk[MAXB];
    int nstore;
    struct blk *start, *rpo[MAXB];
    int nblk;
    struct tmp tmp[MAXT];
    int ntmp;
    bits reg; /* registers the function uses */
};

static struct fn fns[2];
static int nfn;
static struct fn *F; /* the function being built */
static struct blk *B;

static int newtmp(struct fn *f, const char *prefix, int cls)
{
    static int n;
    require(f->ntmp < MAXT, "more temporaries");
    struct tmp *t = &f->tmp[f->ntmp];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%.20s.%d", prefix, ++n);
    snprintf(t->base, sizeof t->base, "%.15s", prefix);
    t->cls = cls;
    return f->ntmp++;
}

static struct ins insb[NINS], *curi;
static void emit(int op, int cls, struct ref to, struct ref a, struct ref b)
{
    require(curi > insb, "more instructions");
    *--curi = (struct ins){op, cls, to, {a, b}};
}
static void emiti(struct ins i) { emit(i.op, i.cls, i.to, i.arg[0], i.arg[1]); }
static void idup(struct blk *b, const struct ins *s, int n)
{
    require(n <= MAXI, "more instructions in a block");
    memmove(b->ins, s, (size_t)n * sizeof *s);
    b->nins = n;
}

/* ------------------------------------------------------------------------------------------
 * Printing, in the style of QBE's own dumps.
 * ---------------------------------------------------------------------------------------- */

static void pref(const struct fn *f, struct ref r)
{
    switch (r.k) {
    case K_TMP:
        P("%%%s", r.v < TMP0 ? reg64[r.v] : f->tmp[r.v].name);
        break;
    case K_INT:
        P("%d", r.v);
        break;
    case K_SYM:
        P("$%s", symname[r.v]);
        break;
    default: /* K_CALL */
        P("call(args %d, result %d)", r.v >> 4, r.v & 1);
        break;
    }
}

static void pins(const struct fn *f, const struct ins *i)
{
    P("\t");
    if (i->to.k != K_NONE) {
        pref(f, i->to);
        P(" =%c ", i->cls ? 'l' : 'w');
    }
    P("%s", opname[i->op]);
    for (int k = 0; k < 2; k++) {
        if (i->arg[k].k != K_NONE) {
            P(k ? ", " : " ");
            pref(f, i->arg[k]);
        }
    }
    P("\n");
}

static void pfn(const struct fn *f)
{
    static const char *const jname[] = {"ret", "ret", "jmp", "jnz", "jlt"};
    P("function $%s() {\n", symname[f->sym]);
    for (int n = 0; n < f->nblk; n++) {
        const struct blk *b = f->rpo[n];
        P("@%s\n", b->name);
        for (int p = 0; p < b->nphi; p++) {
            const struct phi *ph = &b->phi[p];
            P("\t");
            pref(f, ph->to);
            P(" =w phi");
            for (int a = 0; a < ph->narg; a++) {
                P("%s @%s ", a ? "," : "", ph->blk[a]->name);
                pref(f, ph->arg[a]);
            }
            P("\n");
        }
        for (int i = 0; i < b->nins; i++) {
            pins(f, &b->ins[i]);
        }
        P("\t%s", jname[b->jmp.type]);
        const char *sep = " ";
        if (b->jmp.arg.k != K_NONE) {
            P(" ");
            pref(f, b->jmp.arg);
            sep = ", ";
        }
        if (b->s1 != NULL) {
            P("%s@%s", sep, b->s1->name);
        }
        if (b->s2 != NULL) {
            P(", @%s", b->s2->name);
        }
        P("\n");
    }
    P("}\n");
}

static void pset(const struct fn *f, bits s)
{
    for (int t = 1; t < f->ntmp; t++) {
        if (s & BIT(t)) {
            P(" %s", f->tmp[t].name);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_il
 *
 * QBE parses its IL (parse.c). The examples are written here with a few builder calls, in the
 * form the parser would produce. Temporaries are not in SSA form: the loop assigns to %s and
 * %i more than once, and fact assigns to %r in two blocks.
 * ---------------------------------------------------------------------------------------- */

static struct fn *fn_begin(int sym)
{
    F = &fns[nfn++];
    memset(F, 0, sizeof *F);
    F->sym = sym;
    for (int t = 1; t <= R15; t++) {
        snprintf(F->tmp[t].name, sizeof F->tmp[t].name, "%s", reg64[t]);
        F->tmp[t].cls = Kl;
    }
    F->ntmp = TMP0;
    return F;
}

static struct blk *blk_new(struct fn *f, const char *name)
{
    struct blk *b = &f->blk[f->nstore++];
    snprintf(b->name, sizeof b->name, "%s", name);
    if (f->start == NULL) {
        f->start = b;
    }
    return b;
}

/* a temporary by name, created on first use */
static struct ref tv(const char *name)
{
    for (int t = TMP0; t < F->ntmp; t++) {
        if (strcmp(F->tmp[t].base, name) == 0) {
            return tmpr(t);
        }
    }
    return tmpr(newtmp(F, name, Kw));
}

static void at(struct blk *b) { B = b; }

static void add(int op, int cls, struct ref to, struct ref a, struct ref b)
{
    B->ins[B->nins++] = (struct ins){op, cls, to, {a, b}};
}

static void jmp(struct blk *t)
{
    B->jmp.type = Jjmp;
    B->s1 = t;
}

static void jnz(struct ref c, struct blk *t, struct blk *e)
{
    B->jmp.type = Jjnz;
    B->jmp.arg = c;
    B->s1 = t;
    B->s2 = e;
}

static void build_hello(void)
{
    fn_begin(S_MAIN);
    at(blk_new(F, "start"));
    add(Oarg, Kl, NOREF, symr(S_STR), NOREF);
    add(Ocall, Kw, NOREF, symr(S_PRINT_STR), NOREF);
    B->jmp.type = Jret0;
}

static void build_arithmetic(void)
{
    fn_begin(S_MAIN);
    at(blk_new(F, "start"));
    add(Oadd, Kw, tv("a"), intr(2), intr(3));
    add(Omul, Kw, tv("a"), tv("a"), intr(4));
    add(Osub, Kw, tv("a"), tv("a"), intr(6));
    add(Oarg, Kw, NOREF, tv("a"), NOREF);
    add(Ocall, Kw, NOREF, symr(S_PRINT_INT), NOREF);
    B->jmp.type = Jret0;
}

static void build_branch(void)
{
    fn_begin(S_MAIN);
    struct blk *start = blk_new(F, "start"), *then = blk_new(F, "then"),
               *els = blk_new(F, "else"), *end = blk_new(F, "end");
    at(start);
    add(Ocopy, Kw, tv("a"), intr(3), NOREF);
    add(Ocopy, Kw, tv("b"), intr(5), NOREF);
    add(Ocsltw, Kw, tv("c"), tv("a"), tv("b"));
    jnz(tv("c"), then, els);
    at(then);
    add(Oarg, Kw, NOREF, tv("b"), NOREF);
    add(Ocall, Kw, NOREF, symr(S_PRINT_INT), NOREF);
    jmp(end);
    at(els);
    add(Oarg, Kw, NOREF, tv("a"), NOREF);
    add(Ocall, Kw, NOREF, symr(S_PRINT_INT), NOREF);
    jmp(end);
    at(end);
    B->jmp.type = Jret0;
}

static void build_loop(void)
{
    fn_begin(S_MAIN);
    struct blk *start = blk_new(F, "start"), *loop = blk_new(F, "loop"),
               *body = blk_new(F, "body"), *end = blk_new(F, "end");
    at(start);
    add(Ocopy, Kw, tv("s"), intr(0), NOREF);
    add(Ocopy, Kw, tv("i"), intr(1), NOREF);
    jmp(loop);
    at(loop);
    add(Ocopy, Kw, tv("last"), tv("s"), NOREF);
    add(Ocsltw, Kw, tv("c"), tv("i"), intr(11));
    jnz(tv("c"), body, end);
    at(body);
    add(Oadd, Kw, tv("s"), tv("s"), tv("i"));
    add(Oadd, Kw, tv("i"), tv("i"), intr(1));
    jmp(loop);
    at(end);
    add(Oarg, Kw, NOREF, tv("last"), NOREF);
    add(Ocall, Kw, NOREF, symr(S_PRINT_INT), NOREF);
    B->jmp.type = Jret0;
}

static void build_call(void)
{
    fn_begin(S_FACT);
    struct blk *start = blk_new(F, "start"), *base = blk_new(F, "base"), *rec = blk_new(F, "rec"),
               *end = blk_new(F, "end");
    at(start);
    add(Opar, Kw, tv("n"), NOREF, NOREF);
    add(Opar, Kw, tv("one"), NOREF, NOREF);
    add(Ocsltw, Kw, tv("c"), tv("n"), intr(2));
    jnz(tv("c"), base, rec);
    at(base);
    add(Ocopy, Kw, tv("r"), tv("one"), NOREF);
    jmp(end);
    at(rec);
    add(Osub, Kw, tv("m"), tv("n"), intr(1));
    add(Oarg, Kw, NOREF, tv("m"), NOREF);
    add(Oarg, Kw, NOREF, tv("one"), NOREF);
    add(Ocall, Kw, tv("f"), symr(S_FACT), NOREF);
    add(Omul, Kw, tv("r"), tv("n"), tv("f"));
    jmp(end);
    at(end);
    B->jmp.type = Jretw;
    B->jmp.arg = tv("r");

    fn_begin(S_MAIN);
    at(blk_new(F, "start"));
    add(Oarg, Kw, NOREF, intr(5), NOREF);
    add(Oarg, Kw, NOREF, intr(1), NOREF);
    add(Ocall, Kw, tv("x"), symr(S_FACT), NOREF);
    add(Oarg, Kw, NOREF, tv("x"), NOREF);
    add(Ocall, Kw, NOREF, symr(S_PRINT_INT), NOREF);
    B->jmp.type = Jret0;
}

struct program {
    const char *name;
    void (*build)(void);
};
static const struct program programs[] = {
    {"hello", build_hello}, {"arithmetic", build_arithmetic}, {"branch", build_branch},
    {"loop", build_loop},   {"call", build_call},
};

static const struct program *find_program(const char *name)
{
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        if (strcmp(programs[i].name, name) == 0) {
            return &programs[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------
 * The control-flow graph: reverse postorder, predecessors, dominators, frontiers, loops.
 * ---------------------------------------------------------------------------------------- */

static void porec(struct fn *f, struct blk *b, int *npo)
{
    if (b == NULL || b->id != -1) {
        return;
    }
    b->id = 0; /* visited */
    porec(f, b->s1, npo);
    porec(f, b->s2, npo);
    f->rpo[(*npo)++] = b;
}

static void fillcfg(struct fn *f)
{
    for (int i = 0; i < f->nstore; i++) {
        f->blk[i].id = -1;
    }
    f->nblk = 0;
    porec(f, f->start, &f->nblk);
    for (int n = 0; n < f->nblk / 2; n++) {
        struct blk *t = f->rpo[n];
        f->rpo[n] = f->rpo[f->nblk - 1 - n];
        f->rpo[f->nblk - 1 - n] = t;
    }
    for (int n = 0; n < f->nblk; n++) {
        f->rpo[n]->id = n;
        f->rpo[n]->npred = 0;
    }
    for (int n = 0; n < f->nblk; n++) {
        struct blk *b = f->rpo[n];
        if (b->s1) {
            b->s1->pred[b->s1->npred++] = b;
        }
        if (b->s2) {
            b->s2->pred[b->s2->npred++] = b;
        }
    }
}

/* "A Simple, Fast Dominance Algorithm", Cooper, Harvey and Kennedy */
static struct blk *inter(struct blk *b1, struct blk *b2)
{
    if (b1 == NULL) {
        return b2;
    }
    while (b1 != b2) {
        while (b1->id > b2->id) {
            b1 = b1->idom;
        }
        while (b2->id > b1->id) {
            b2 = b2->idom;
        }
    }
    return b1;
}

static void filldom(struct fn *f)
{
    for (int n = 0; n < f->nblk; n++) {
        f->rpo[n]->idom = f->rpo[n]->dom = f->rpo[n]->dlink = NULL;
    }
    int ch;
    do {
        ch = 0;
        for (int n = 1; n < f->nblk; n++) {
            struct blk *b = f->rpo[n], *d = NULL;
            for (int p = 0; p < b->npred; p++) {
                if (b->pred[p]->idom || b->pred[p] == f->start) {
                    d = inter(d, b->pred[p]);
                }
            }
            if (d != b->idom) {
                ch++;
                b->idom = d;
            }
        }
    } while (ch);
    for (int n = 0; n < f->nblk; n++) {
        struct blk *b = f->rpo[n], *d = b->idom;
        if (d) {
            b->dlink = d->dom;
            d->dom = b;
        }
    }
}

static int sdom(struct blk *b1, struct blk *b2)
{
    if (b1 == b2) {
        return 0;
    }
    while (b2->id > b1->id) {
        b2 = b2->idom;
    }
    return b1 == b2;
}

static void fillfron(struct fn *f)
{
    for (int n = 0; n < f->nblk; n++) {
        f->rpo[n]->nfron = 0;
    }
    for (int n = 0; n < f->nblk; n++) {
        struct blk *b = f->rpo[n];
        struct blk *s[2] = {b->s1, b->s2};
        for (int k = 0; k < 2; k++) {
            if (s[k]) {
                for (struct blk *a = b; !sdom(a, s[k]); a = a->idom) {
                    a->fron[a->nfron++] = s[k];
                }
            }
        }
    }
}

/* a block that is in a loop has its loop depth multiplied by 10 for every loop around it */
static void fillloop(struct fn *f)
{
    for (int n = 0; n < f->nblk; n++) {
        f->rpo[n]->loop = 1;
        f->rpo[n]->visit = -1;
    }
    for (int n = 0; n < f->nblk; n++) {
        struct blk *hd = f->rpo[n];
        for (int p = 0; p < hd->npred; p++) {
            if (hd->pred[p]->id >= n) {
                /* mark the natural loop of the back edge: walk the predecessors up to hd */
                struct blk *stack[MAXB];
                int sp = 0;
                stack[sp++] = hd->pred[p];
                while (sp > 0) {
                    struct blk *b = stack[--sp];
                    if (b->id < hd->id || b->visit == hd->id) {
                        continue;
                    }
                    b->visit = hd->id;
                    b->loop *= 10;
                    for (int q = 0; q < b->npred; q++) {
                        stack[sp++] = b->pred[q];
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Uses, definitions and liveness.
 * ---------------------------------------------------------------------------------------- */

static int phicls(struct fn *f, int t)
{
    int t1 = f->tmp[t].phi;
    if (!t1) {
        return t;
    }
    t1 = phicls(f, t1);
    f->tmp[t].phi = t1;
    return t1;
}

static void use(struct fn *f, struct ref r, int bid)
{
    if (istmp(r)) {
        f->tmp[r.v].nuse++;
        f->tmp[r.v].usebm |= BIT(bid);
    }
}

/* fill the definition count, use count, blocks of the uses and the PHI classes: a PHI and its
 * arguments share a class, which the allocator uses to give them one hint */
static void filluse(struct fn *f)
{
    for (int t = TMP0; t < f->ntmp; t++) {
        struct tmp *tm = &f->tmp[t];
        tm->ndef = tm->nuse = tm->phi = 0;
        tm->bid = -1;
        tm->usebm = 0;
    }
    for (int n = 0; n < f->nblk; n++) {
        struct blk *b = f->rpo[n];
        for (int p = 0; p < b->nphi; p++) {
            struct phi *ph = &b->phi[p];
            f->tmp[ph->to.v].bid = b->id;
            f->tmp[ph->to.v].ndef++;
            int tp = phicls(f, ph->to.v);
            for (int a = 0; a < ph->narg; a++) {
                use(f, ph->arg[a], b->id);
                /* join the class of the PHI, unless the argument is in it already */
                int ta = phicls(f, ph->arg[a].v);
                f->tmp[ta].phi = tp * (ta != tp);
            }
        }
        for (int i = 0; i < b->nins; i++) {
            struct ins *in = &b->ins[i];
            if (istmp(in->to)) {
                f->tmp[in->to.v].bid = b->id;
                f->tmp[in->to.v].ndef++;
            }
            use(f, in->arg[0], b->id);
            use(f, in->arg[1], b->id);
        }
        use(f, b->jmp.arg, b->id);
    }
}

static bits liveon(struct blk *b, struct blk *s)
{
    bits v = s->in;
    for (int p = 0; p < s->nphi; p++) {
        v &= ~BIT(s->phi[p].to.v);
    }
    for (int p = 0; p < s->nphi; p++) {
        for (int a = 0; a < s->phi[p].narg; a++) {
            if (s->phi[p].blk[a] == b) {
                v |= BIT(s->phi[p].arg[a].v);
                b->gen |= BIT(s->phi[p].arg[a].v);
            }
        }
    }
    return v;
}

static void bset(struct blk *b, struct ref r)
{
    if (istmp(r)) {
        b->gen |= BIT(r.v);
        b->in |= BIT(r.v);
    }
}

/* liveness analysis: in, out and gen of every block. A call defines the registers of its
 * result and uses the registers of its arguments, so those registers are live in between. */
static void filllive(struct fn *f)
{
    for (int n = 0; n < f->nblk; n++) {
        f->rpo[n]->in = f->rpo[n]->out = f->rpo[n]->gen = 0;
    }
    int chg;
    do {
        chg = 0;
        for (int n = f->nblk - 1; n >= 0; n--) {
            struct blk *b = f->rpo[n];
            bits u = b->out;
            if (b->s1) {
                b->out |= liveon(b, b->s1);
            }
            if (b->s2) {
                b->out |= liveon(b, b->s2);
            }
            chg |= b->out != u;
            b->in = b->out;
            if (b->jmp.arg.k == K_CALL) {
                b->in |= retregs(b->jmp.arg);
            } else {
                bset(b, b->jmp.arg);
            }
            for (int i = b->nins; i-- > 0;) {
                struct ins *in = &b->ins[i];
                if (in->op == Ocall) {
                    b->in &= ~retregs(in->arg[1]);
                    b->in |= argregs(in->arg[1]);
                }
                if (istmp(in->to)) {
                    b->gen |= BIT(in->to.v);
                    b->in &= ~BIT(in->to.v);
                }
                bset(b, in->arg[0]);
                bset(b, in->arg[1]);
            }
        }
    } while (chg);
}

/* ------------------------------------------------------------------------------------------
 * stage_il
 * ---------------------------------------------------------------------------------------- */

static void stage_il(void)
{
    P("== stage_il: the IL as a frontend writes it, temporaries are mutable ==\n");
    for (int i = 0; i < nfn; i++) {
        fillcfg(&fns[i]);
        pfn(&fns[i]);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_ssa
 *
 * A temporary that is assigned in several blocks needs a PHI wherever two of those
 * assignments meet. Those places are the dominance frontiers of the blocks that assign it. QBE
 * builds pruned SSA: a PHI is only inserted where the temporary is live on entry, and an
 * assignment whose value does not leave its block just gets a new name. Then every use is
 * renamed to the dominating definition.
 * ---------------------------------------------------------------------------------------- */

static int refindex(struct fn *f, int t) { return newtmp(f, f->tmp[t].name, f->tmp[t].cls); }

static void phiins(struct fn *f)
{
    int nt = f->ntmp;
    for (int t = TMP0; t < nt; t++) {
        struct tmp *tm = &f->tmp[t];
        tm->visit = 0;
        /* one definition whose uses are all in its own block, or that dominates everything */
        if (tm->ndef == 1 && ((tm->usebm & ~BIT(tm->bid)) == 0 || tm->bid == f->start->id)) {
            continue;
        }
        bits defs = 0;
        for (int n = 0; n < f->nblk; n++) {
            struct blk *b = f->rpo[n];
            int cur = t; /* the name the temporary has at this point of the block */
            b->visit = 0;
            for (int i = 0; i < b->nins; i++) {
                struct ins *in = &b->ins[i];
                if (req(in->arg[0], tmpr(t))) {
                    in->arg[0] = tmpr(cur);
                }
                if (req(in->arg[1], tmpr(t))) {
                    in->arg[1] = tmpr(cur);
                }
                if (req(in->to, tmpr(t))) {
                    if (b->out & BIT(t)) {
                        defs |= BIT(b->id);
                    } else {
                        cur = refindex(f, t);
                        in->to = tmpr(cur);
                    }
                }
            }
            if (req(b->jmp.arg, tmpr(t))) {
                b->jmp.arg = tmpr(cur);
            }
        }
        struct blk *wl[2 * MAXB];
        int sp = 0;
        for (int n = 0; n < f->nblk; n++) {
            if (defs & BIT(n)) {
                wl[sp++] = f->rpo[n];
            }
        }
        while (sp > 0) {
            tm->visit = t;
            struct blk *b = wl[--sp];
            for (int k = 0; k < b->nfron; k++) {
                struct blk *a = b->fron[k];
                if (a->visit++ == 0 && (a->in & BIT(t))) {
                    memmove(&a->phi[1], &a->phi[0], (size_t)a->nphi * sizeof a->phi[0]);
                    a->phi[0] = (struct phi){.to = tmpr(t), .cls = tm->cls};
                    a->nphi++;
                    wl[sp++] = a;
                }
            }
        }
    }
}

struct name {
    int t;
    struct blk *b;
};
static struct name stk[MAXT][6];
static int nstk[MAXT];

static void rendef(struct fn *f, struct ref *r, struct blk *b)
{
    if (!istmp(*r) || !f->tmp[r->v].visit) {
        return;
    }
    int t = r->v, r1 = refindex(f, t);
    f->tmp[r1].visit = t;
    stk[t][nstk[t]++] = (struct name){r1, b};
    *r = tmpr(r1);
}

/* the innermost name of t that is visible in b */
static struct ref getstk(int t)
{
    require(nstk[t] > 0, "a temporary that is used before it is defined");
    return tmpr(stk[t][nstk[t] - 1].t);
}

static void renuse(struct fn *f, struct ref *r)
{
    if (istmp(*r) && f->tmp[r->v].visit) {
        *r = getstk(r->v);
    }
}

static void renblk(struct fn *f, struct blk *b)
{
    int saved[MAXT];
    memcpy(saved, nstk, sizeof nstk);
    for (int p = 0; p < b->nphi; p++) {
        rendef(f, &b->phi[p].to, b);
    }
    for (int i = 0; i < b->nins; i++) {
        renuse(f, &b->ins[i].arg[0]);
        renuse(f, &b->ins[i].arg[1]);
        rendef(f, &b->ins[i].to, b);
    }
    renuse(f, &b->jmp.arg);
    struct blk *succ[2] = {b->s1, b->s2};
    for (int k = 0; k < 2; k++) {
        if (succ[k] == NULL) {
            continue;
        }
        for (int p = 0; p < succ[k]->nphi; p++) {
            struct phi *ph = &succ[k]->phi[p];
            int t = f->tmp[ph->to.v].visit;
            ph->arg[ph->narg] = getstk(t);
            ph->blk[ph->narg++] = b;
        }
    }
    for (struct blk *s = b->dom; s; s = s->dlink) {
        renblk(f, s);
    }
    memcpy(nstk, saved, sizeof nstk); /* the names defined in b are not visible outside it */
}

static void stage_ssa(void)
{
    P("== stage_ssa: dominators, frontiers, pruned phi insertion, renaming ==\n");
    for (int i = 0; i < nfn; i++) {
        struct fn *f = &fns[i];
        filluse(f);
        filldom(f);
        fillfron(f);
        filllive(f);
        P("$%s: dominator tree and dominance frontiers\n", symname[f->sym]);
        for (int n = 0; n < f->nblk; n++) {
            struct blk *b = f->rpo[n];
            P("\t@%-6s idom %-6s frontier", b->name, b->idom ? b->idom->name : "-");
            for (int k = 0; k < b->nfron; k++) {
                P(" @%s", b->fron[k]->name);
            }
            P("\n");
        }
        phiins(f);
        memset(nstk, 0, sizeof nstk);
        renblk(f, f->start);
        pfn(f);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_abi
 *
 * The first thing the amd64 System V ABI code does is turn parameters, call arguments and
 * return values into plain copies to and from the registers they travel in. From here on the
 * rest of the backend only sees copies, and the register allocator decides where everything
 * else goes.
 * ---------------------------------------------------------------------------------------- */

static void stage_abi(void)
{
    P("== stage_abi: parameters, arguments and results become copies to and from registers ==\n");
    for (int fi = 0; fi < nfn; fi++) {
        struct fn *f = &fns[fi];
        for (int n = 0; n < f->nblk; n++) {
            struct blk *b = f->rpo[n];
            struct ins out[MAXI];
            struct ref args[MAXP];
            int acls[MAXP];
            int nout = 0, nargs = 0, npar = 0;
            for (int i = 0; i < b->nins; i++) {
                struct ins *in = &b->ins[i];
                if (in->op == Opar) {
                    out[nout++] = (struct ins){Ocopy, in->cls, in->to, {tmpr(rsave[npar++])}};
                } else if (in->op == Oarg) {
                    acls[nargs] = in->cls;
                    args[nargs++] = in->arg[0];
                } else if (in->op == Ocall) {
                    for (int k = 0; k < nargs; k++) {
                        out[nout++] = (struct ins){Ocopy, acls[k], tmpr(rsave[k]), {args[k]}};
                    }
                    /* the copy out of rax is there even if nothing uses the result: the
                     * allocator finds the registers a call clobbers through it */
                    out[nout++] = (struct ins){Ocall, Kw, NOREF, {in->arg[0], callr(nargs << 4 | 1)}};
                    out[nout++] = (struct ins){Ocopy, Kw, in->to, {tmpr(RAX)}};
                    nargs = 0;
                } else {
                    out[nout++] = *in;
                }
            }
            idup(b, out, nout);
            if (b->jmp.type == Jretw) {
                b->ins[b->nins++] = (struct ins){Ocopy, Kw, tmpr(RAX), {b->jmp.arg}};
                b->jmp.type = Jret0;
                b->jmp.arg = callr(1);
            }
        }
        pfn(f);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_isel
 *
 * Instruction selection walks each block backwards, emitting into a buffer that grows towards
 * the front, so when it reaches an instruction it already knows what comes after. The jump
 * is selected first: a compare that feeds only the jump right after it is fused into it, as
 * a flag-setting xcmp and a conditional jump. An address constant that is copied becomes a
 * lea (addr).
 * ---------------------------------------------------------------------------------------- */

static void seljmp(struct fn *f, struct blk *b)
{
    if (b->jmp.type != Jjnz) {
        return;
    }
    struct ins *last = &b->ins[b->nins - 1];
    require((last->op == Ocsltw) & req(last->to, b->jmp.arg) & (f->tmp[last->to.v].nuse == 1),
            "a jump on anything but a compare that directly precedes it");
    emit(Oxcmp, Kw, NOREF, last->arg[0], last->arg[1]);
    b->nins--;
    b->jmp.type = Jjlt;
    b->jmp.arg = NOREF;
}

static void sel(struct ins i)
{
    if (i.op == Ocopy && i.arg[0].k == K_SYM) {
        i.op = Oaddr;
    }
    emiti(i);
}

static void stage_isel(void)
{
    P("== stage_isel: compare-and-branch fusion, address constants become lea ==\n");
    for (int fi = 0; fi < nfn; fi++) {
        struct fn *f = &fns[fi];
        filluse(f);
        for (int n = 0; n < f->nblk; n++) {
            struct blk *b = f->rpo[n];
            curi = &insb[NINS];
            seljmp(f, b);
            for (int i = b->nins; i-- > 0;) {
                sel(b->ins[i]);
            }
            idup(b, curi, (int)(&insb[NINS] - curi));
        }
        pfn(f);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_live
 *
 * Liveness is computed again on the selected code, then every block gets a loop depth (each
 * loop around it multiplies a weight by ten) and every temporary a cost: the sum of the
 * weights of the blocks that define or use it. The allocator uses the costs to decide which
 * temporaries are the most worth a good register.
 * ---------------------------------------------------------------------------------------- */

static void fillcost(struct fn *f)
{
    for (int t = TMP0; t < f->ntmp; t++) {
        f->tmp[t].cost = 0;
    }
    for (int n = 0; n < f->nblk; n++) {
        struct blk *b = f->rpo[n];
        for (int i = 0; i < b->nins; i++) {
            struct ref r[3] = {b->ins[i].to, b->ins[i].arg[0], b->ins[i].arg[1]};
            for (int k = 0; k < 3; k++) {
                if (istmp(r[k]) && r[k].v >= TMP0) {
                    f->tmp[r[k].v].cost += b->loop;
                }
            }
        }
    }
}

static void stage_live(void)
{
    P("== stage_live: liveness, loop depth and the cost of every temporary ==\n");
    for (int fi = 0; fi < nfn; fi++) {
        struct fn *f = &fns[fi];
        filluse(f);
        filllive(f);
        fillloop(f);
        fillcost(f);
        P("$%s:\n", symname[f->sym]);
        for (int n = 0; n < f->nblk; n++) {
            struct blk *b = f->rpo[n];
            P("\t@%-6s weight %-3d live in {", b->name, b->loop);
            pset(f, b->in & ~REGS);
            P(" } out {");
            pset(f, b->out & ~REGS);
            P(" }\n");
        }
        P("\tcosts:");
        for (int t = TMP0; t < f->ntmp; t++) {
            if (f->tmp[t].cost > 0) {
                P(" %s=%d", f->tmp[t].name, f->tmp[t].cost);
            }
        }
        P("\n");
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_spill
 *
 * The real spill pass keeps at most as many temporaries live as there are registers and
 * stores the rest in stack slots. This entry never runs out of registers, but it keeps the
 * other thing the pass does: when it walks past a call it tells the allocator, through
 * hint.m, that every temporary that lives across the call must not be given a caller-save
 * register, because the call destroys those.
 * ---------------------------------------------------------------------------------------- */

static void stage_spill(void)
{
    P("== stage_spill: temporaries that live across a call must avoid caller-save registers ==\n");
    bits caller = 0;
    for (int k = 0; rsave[k] >= 0; k++) {
        caller |= BIT(rsave[k]);
    }
    for (int fi = 0; fi < nfn; fi++) {
        struct fn *f = &fns[fi];
        for (int n = 0; n < f->nblk; n++) {
            struct blk *b = f->rpo[n];
            if (b->jmp.arg.k == K_CALL) {
                b->out |= retregs(b->jmp.arg); /* the value being returned is live at the end */
            }
            bits live = b->out;
            for (int i = b->nins; i-- > 0;) {
                struct ins *in = &b->ins[i];
                if (in->op == Ocall) {
                    for (int t = TMP0; t < f->ntmp; t++) {
                        if (live & BIT(t)) {
                            f->tmp[phicls(f, t)].avoid |= caller;
                        }
                    }
                    live &= ~retregs(in->arg[1]);
                    live |= argregs(in->arg[1]);
                }
                if (istmp(in->to)) {
                    live &= ~BIT(in->to.v);
                }
                for (int k = 0; k < 2; k++) {
                    if (istmp(in->arg[k])) {
                        live |= BIT(in->arg[k].v);
                    }
                }
            }
        }
        P("$%s:", symname[f->sym]);
        for (int t = TMP0; t < f->ntmp; t++) {
            if (f->tmp[t].avoid) {
                P(" %s", f->tmp[t].name);
            }
        }
        P("\n");
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_rega
 *
 * Registers are assigned block by block, innermost loops first, and each block is walked
 * backwards. At the end of a block the live temporaries get registers (the ones a successor
 * already chose, then the ones with hints, then the costly ones). Walking up, a use gives a
 * temporary a register if it has none and a definition frees it. A copy to a register hints
 * that its source would like that register. A run of copies from registers (the parameters,
 * or the result of a call) is handled as one parallel move. Afterwards, wherever two blocks
 * disagree about where a temporary lives, a move is added: at the start of a block that has
 * several predecessors if they all agree, otherwise on the edge, in a new block.
 * ---------------------------------------------------------------------------------------- */

#define NREGMAP 16
struct rmap {
    int t[NREGMAP], r[NREGMAP]; /* temporary or register i lives in register r[i] */
    bits b;                     /* the temporaries and registers that are mapped */
    int n;
};

static struct fn *RF;
static int curloop;
static bits regu;

static struct tmp *tcls(int t) { return &RF->tmp[phicls(RF, t)]; }

static void sethint(int t, int r)
{
    struct tmp *p = tcls(t);
    if (p->hint_r == 0 || p->hint_w > curloop) {
        p->hint_r = r;
        p->hint_w = curloop;
        RF->tmp[t].last = 0;
    }
}

static int rfind(const struct rmap *m, int t)
{
    for (int i = 0; i < m->n; i++) {
        if (m->t[i] == t) {
            return m->r[i];
        }
    }
    return 0;
}

static void radd(struct rmap *m, int t, int r)
{
    m->b |= BIT(t) | BIT(r);
    m->t[m->n] = t;
    m->r[m->n++] = r;
    regu |= BIT(r);
}

static int rfree(struct rmap *m, int t)
{
    require(m->b & BIT(t), "freeing a temporary that has no register");
    int i = 0;
    while (m->t[i] != t) {
        i++;
    }
    int r = m->r[i];
    m->b &= ~(BIT(t) | BIT(r));
    m->n--;
    memmove(&m->t[i], &m->t[i + 1], (size_t)(m->n - i) * sizeof m->t[0]);
    memmove(&m->r[i], &m->r[i + 1], (size_t)(m->n - i) * sizeof m->r[0]);
    return r;
}

/* the register of t: the one it already has, the one it had in another block, the one its
 * hint asks for, or else the first free one that is not forbidden. try only takes the first
 * three */
static int ralloctry(struct rmap *m, int t, int try)
{
    int r = rfind(m, t);
    if (r) {
        return r;
    }
    r = RF->tmp[t].last;
    if (r == 0 || (m->b & BIT(r))) {
        r = tcls(t)->hint_r;
    }
    if (r == 0 || (m->b & BIT(r))) {
        if (try) {
            return 0;
        }
        bits regs = tcls(t)->avoid | m->b;
        r = RAX;
        while (regs & BIT(r)) {
            r++;
        }
        require(r <= R15, "a register that the temporary may use");
    }
    radd(m, t, r);
    sethint(t, r);
    RF->tmp[t].last = r;
    return r;
}

static int ralloc(struct rmap *m, int t) { return ralloctry(m, t, 0); }

/* a parallel move: all the sources are read before any destination is written */
#define NPM 16
static struct {
    struct ref src, dst;
    int cls;
} pm[NPM];
static int npm;

static void pmadd(int src, int dst, int cls)
{
    require(npm < NPM, "more moves in one parallel move");
    pm[npm].src = tmpr(src);
    pm[npm].dst = tmpr(dst);
    pm[npm++].cls = cls;
}

/* Emission is backwards, so the moves are emitted last to first. QBE orders moves that depend on
 * each other and breaks a cycle with a swap. Here the moves must be independent. */
static void pmgen(void)
{
    for (int i = 0; i < npm; i++) {
        for (int j = 0; j < npm; j++) {
            require((i == j) | req(pm[j].src, pm[j].dst) | !req(pm[j].dst, pm[i].src),
                    "moves that depend on each other, which QBE orders or swaps");
        }
    }
    for (int i = npm; i-- > 0;) {
        if (!req(pm[i].src, pm[i].dst)) {
            emit(Ocopy, pm[i].cls, pm[i].dst, pm[i].src, NOREF);
        }
    }
}

/* make register r the home of `to` for the copy `to = copy r`: to is mapped to r in the map
 * for before the copy, and the parallel move that follows carries it to where it lives after */
static void move(int r, struct ref to, struct rmap *m)
{
    int t = r;
    if (to.k != K_NONE) {
        rfree(m, to.v);
        t = to.v;
    }
    if (m->b & BIT(r)) {
        /* r is in use, and not by `to`: that temporary goes to another register */
        int u = 0; /* the one entry that is mapped to r */
        for (int n = 0; n < m->n; n++) {
            u += (m->r[n] == r) * m->t[n];
        }
        rfree(m, u);
        m->b |= BIT(r);
        ralloc(m, u);
        m->b &= ~BIT(r);
    }
    radd(m, t, r);
}

static int regcpy(const struct ins *i) { return i->op == Ocopy && isreg(i->arg[0]); }

/* i is the index of the last of a run of copies from registers; returns the first */
static int dopm(struct blk *b, int i, struct rmap *m)
{
    struct rmap m0 = *m;
    int first = i + 1;
    for (int k = i, run = 1; k >= 0; k--) {
        run &= regcpy(&b->ins[k]);
        first -= run;
    }
    for (int k = i; k >= first; k--) {
        move(b->ins[k].arg[0].v, b->ins[k].to, m);
    }
    if (first != 0) {
        require(b->ins[first - 1].op == Ocall, "a run of copies from registers that starts mid-block");
        bits def = retregs(b->ins[first - 1].arg[1]);
        for (int k = 0; rsave[k] >= 0; k++) {
            if (!(BIT(rsave[k]) & def)) {
                move(rsave[k], NOREF, m);
            }
        }
    }
    npm = 0;
    for (int n = 0; n < m->n; n++) {
        int r = rfind(&m0, m->t[n]);
        if (r) {
            pmadd(m->r[n], r, RF->tmp[m->t[n]].cls);
        }
    }
    for (int k = first; k <= i; k++) {
        if (b->ins[k].to.k != K_NONE) {
            rfree(m, b->ins[k].to.v);
        }
        int r = b->ins[k].arg[0].v;
        if (!rfind(m, r)) {
            radd(m, r, r);
        }
    }
    pmgen();
    return first;
}

static void doblk(struct blk *b, struct rmap *cur)
{
    curi = &insb[NINS];
    for (int i1 = b->nins; i1 > 0;) {
        emiti(b->ins[--i1]);
        struct ins *i = curi;
        if (i->op == Ocall) {
            /* the call keeps its argument registers, and destroys every other caller-save one */
            bits rs = argregs(i->arg[1]);
            for (int k = 0; rsave[k] >= 0; k++) {
                if (!(rs & BIT(rsave[k]))) {
                    rfree(cur, rsave[k]);
                }
            }
            continue;
        }
        if (i->op == Ocopy) {
            if (regcpy(i)) {
                curi++;
                i1 = dopm(b, i1, cur);
                continue;
            }
            if (isreg(i->to) && istmp(i->arg[0])) {
                sethint(i->arg[0].v, i->to.v);
            }
        }
        if (i->to.k != K_NONE) {
            int rf = rfree(cur, i->to.v);
            require(rf != 0, "a definition that is used");
            i->to = tmpr(rf);
        }
        for (int x = 0; x < 2; x++) {
            if (istmp(i->arg[x])) {
                i->arg[x] = tmpr(ralloc(cur, i->arg[x].v));
            }
        }
        if (i->op == Ocopy && req(i->to, i->arg[0])) {
            curi++;
        }
    }
    idup(b, curi, (int)(&insb[NINS] - curi));
}

/* the order the blocks are done in: deepest loops first, and later blocks before earlier ones */
static int carve(const void *a, const void *b)
{
    const struct blk *ba = *(struct blk *const *)a, *bb = *(struct blk *const *)b;
    int ka = ba->loop * 64 + ba->id, kb = bb->loop * 64 + bb->id;
    return (kb > ka) - (kb < ka);
}

/* the order of the temporaries live at the end of a block: those that have a register already,
 * then those with a hint, then the more costly, and the earlier ones in a tie */
static int prio2(const void *a, const void *b)
{
    int t1 = *(const int *)a, t2 = *(const int *)b;
    int d = (RF->tmp[t2].last != 0) * 2 + (tcls(t2)->hint_r != 0) -
            (RF->tmp[t1].last != 0) * 2 - (tcls(t1)->hint_r != 0);
    d = d ? d : RF->tmp[t2].cost - RF->tmp[t1].cost;
    return d ? d : t1 - t2;
}

static struct rmap bmap[MAXB], emap[MAXB];

static void pmap(const char *what, const struct fn *f, const struct rmap *m)
{
    P("%s", what);
    for (int i = 0; i < m->n; i++) {
        if (m->t[i] >= TMP0) {
            P(" %s:%s", f->tmp[m->t[i]].name, reg64[m->r[i]]);
        }
    }
    P("\n");
}

static void rega(struct fn *f)
{
    RF = f;
    regu = 0;
    memset(bmap, 0, sizeof bmap);
    memset(emap, 0, sizeof emap);
    curloop = 1 << 30;
    for (int t = 0; t < f->ntmp; t++) {
        f->tmp[t].hint_r = t < TMP0 ? t : 0;
        f->tmp[t].hint_w = curloop;
        f->tmp[t].last = 0;
    }
    /* order the blocks */
    struct blk *order[MAXB];
    memcpy(order, f->rpo, (size_t)f->nblk * sizeof order[0]);
    qsort(order, (size_t)f->nblk, sizeof order[0], carve);
    /* the parameters, which the ABI code copied out of registers, hint the registers */
    for (int i = 0, run = 1; i < f->start->nins; i++) {
        run &= regcpy(&f->start->ins[i]);
        if (run) {
            sethint(f->start->ins[i].to.v, f->start->ins[i].arg[0].v);
        }
    }

    /* 1. assign registers */
    for (int k = 0; k < f->nblk; k++) {
        struct blk *b = order[k];
        struct rmap cur = {0};
        int rl[MAXT], x = 0;
        curloop = b->loop;
        for (int t = TMP0; t < f->ntmp; t++) {
            if (b->out & BIT(t)) {
                rl[x++] = t;
            }
        }
        qsort(rl, (size_t)x, sizeof rl[0], prio2);
        for (int r = RAX; r <= R15; r++) {
            if (b->out & BIT(r)) {
                radd(&cur, r, r);
            }
        }
        for (int j = 0; j < x; j++) {
            ralloctry(&cur, rl[j], 1);
        }
        for (int j = 0; j < x; j++) {
            ralloc(&cur, rl[j]);
        }
        emap[b->id] = cur;
        doblk(b, &cur);
        bmap[b->id] = cur;
        b->in = cur.b;
        for (int p = 0; p < b->nphi; p++) {
            b->in &= ~BIT(b->phi[p].to.v);
        }
    }

    for (int n = 0; n < f->nblk; n++) {
        P("\t@%s\n", f->rpo[n]->name);
        pmap("\t\tbegins with", f, &bmap[f->rpo[n]->id]);
        pmap("\t\tends with  ", f, &emap[f->rpo[n]->id]);
    }

    /* 2. the copies go on the edges, in new blocks */
    int nold = f->nblk;
    for (int n = 0; n < nold; n++) {
        struct blk *b = f->rpo[n];
        struct blk **ps[2] = {&b->s1, &b->s2};
        for (int k = 0; k < 2; k++) {
            struct blk *s = *ps[k];
            if (s == NULL) {
                continue;
            }
            npm = 0;
            for (int p = 0; p < s->nphi; p++) {
                int r = rfind(&bmap[s->id], s->phi[p].to.v);
                int u = 0;
                while (s->phi[p].blk[u] != b) {
                    u++;
                }
                pmadd(rfind(&emap[b->id], s->phi[p].arg[u].v), r, s->phi[p].cls);
            }
            for (int t = TMP0; t < f->ntmp; t++) {
                if (s->in & BIT(t)) {
                    pmadd(rfind(&emap[b->id], t), rfind(&bmap[s->id], t), f->tmp[t].cls);
                }
            }
            curi = &insb[NINS];
            pmgen();
            if (curi == &insb[NINS]) {
                continue;
            }
            require(f->nstore < MAXB, "more blocks");
            struct blk *b1 = &f->blk[f->nstore++];
            memset(b1, 0, sizeof *b1);
            snprintf(b1->name, sizeof b1->name, "%.15s_%.15s", b->name, s->name);
            b1->loop = (b->loop + s->loop) / 2;
            idup(b1, curi, (int)(&insb[NINS] - curi));
            b1->jmp.type = Jjmp;
            b1->s1 = s;
            b1->id = f->nblk;
            f->rpo[f->nblk++] = b1;
            *ps[k] = b1;
        }
    }
    for (int n = 0; n < f->nblk; n++) {
        f->rpo[n]->nphi = 0;
    }
    f->reg = regu;
}

static void stage_rega(void)
{
    P("== stage_rega: hints, backward allocation, parallel moves, edge blocks ==\n");
    for (int fi = 0; fi < nfn; fi++) {
        P("$%s: register maps at the start and the end of every block\n", symname[fns[fi].sym]);
        rega(&fns[fi]);
        pfn(&fns[fi]);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_emit
 *
 * Jump threading comes first: a block with no instructions that only jumps is skipped by
 * everything that jumps to it, and every return becomes a jump to one final block that holds
 * the epilogue. Then each function is written out in block order. The prologue saves the
 * frame pointer and the callee-save registers the allocator used, with one extra 8 bytes
 * when there is an odd number of them so that calls stay 16-byte aligned. Instructions are
 * two-address on amd64, so `to = op a, b` becomes `mov a, to; op b, to`. QBE writes assembly
 * text for an assembler, this entry writes the bytes too.
 * ---------------------------------------------------------------------------------------- */

static uint8_t code[2048];
static int clen;
#define MAXLINES 200
static struct {
    int off, label;
    char text[48];
} lines[MAXLINES];
static int nlines;

enum { FIX_BLOCK, FIX_FUNC, FIX_PRINT_INT, FIX_PRINT_STR, FIX_STRING };
static struct {
    int at, kind, fn, target;
} fixups[32];
static int nfix;
static int blkoff[2][MAXB], fnoff[2];
static int print_int_off, print_str_off, string_off;

#define TEXT(is_label, ...)                                                                        \
    do {                                                                                           \
        require(nlines < MAXLINES, "more lines of assembly");                                      \
        lines[nlines].off = clen;                                                                  \
        lines[nlines].label = (is_label);                                                          \
        snprintf(lines[nlines++].text, sizeof lines[0].text, __VA_ARGS__);                         \
    } while (0)

static void byte(int v)
{
    require(clen < (int)sizeof code, "more machine code");
    code[clen++] = (uint8_t)v;
}

static void imm32(int v)
{
    for (int k = 0; k < 4; k++) {
        byte(v >> (8 * k));
    }
}

static void fixup(int kind, int fn, int target)
{
    require(nfix < 32, "more fixups");
    fixups[nfix].at = clen;
    fixups[nfix].kind = kind;
    fixups[nfix].fn = fn;
    fixups[nfix++].target = target;
    imm32(0);
}

static int fits8(int v) { return (unsigned)(v + 128) < 256; }

static int hw(struct ref r)
{
    require(hwreg[r.v] < 8, "a register below r8, the encodings carry no REX bits for the rest");
    return hwreg[r.v];
}

static void modrm(int reg, int rm) { byte(0xc0 | reg << 3 | rm); }

/* a copy, in either of its two forms */
static void emit_mov(struct ref to, struct ref a)
{
    if (a.k == K_INT) {
        TEXT(0, "movl $%d, %%%s", a.v, reg32[to.v]);
        byte(0xb8 + hw(to));
        imm32(a.v);
    } else {
        TEXT(0, "movl %%%s, %%%s", reg32[a.v], reg32[to.v]);
        byte(0x89);
        modrm(hw(a), hw(to));
    }
}

/* add, sub and imul, with the two-address constraint */
static void emit_alu(const struct ins *i)
{
    struct ref to = i->to, a = i->arg[0], b = i->arg[1];
    static const char *const nm[] = {"addl", "subl", "imull"};
    const char *name = nm[i->op - Oadd];
    if (i->op == Omul && b.k == K_INT) {
        /* the three-address form of a multiplication by a constant */
        require(fits8(b.v), "an immediate that fits in 8 bits");
        TEXT(0, "imull $%d, %%%s, %%%s", b.v, reg32[a.v], reg32[to.v]);
        byte(0x6b);
        modrm(hw(to), hw(a));
        byte(b.v);
        return;
    }
    if (req(to, b)) {
        require(i->op != Osub, "the negation trick for a subtraction into its second operand");
        struct ref t = a;
        a = b;
        b = t;
    }
    if (!req(to, a)) {
        emit_mov(to, a);
    }
    if (b.k == K_INT) {
        require(fits8(b.v), "an immediate that fits in 8 bits");
        TEXT(0, "%s $%d, %%%s", name, b.v, reg32[to.v]);
        byte(0x83);
        modrm(i->op == Oadd ? 0 : 5, hw(to));
        byte(b.v);
    } else if (i->op == Omul) {
        TEXT(0, "imull %%%s, %%%s", reg32[b.v], reg32[to.v]);
        byte(0x0f);
        byte(0xaf);
        modrm(hw(to), hw(b));
    } else {
        require(i->op == Oadd, "a subtraction of two registers");
        TEXT(0, "%s %%%s, %%%s", name, reg32[b.v], reg32[to.v]);
        byte(0x01);
        modrm(hw(b), hw(to));
    }
}

static void emit_ins(int fi, const struct ins *i)
{
    switch (i->op) {
    case Ocopy:
        emit_mov(i->to, i->arg[0]);
        break;
    case Oaddr:
        TEXT(0, "leaq %s(%%rip), %%%s", symname[i->arg[0].v], reg64[i->to.v]);
        byte(0x48);
        byte(0x8d);
        byte(hw(i->to) << 3 | 5);
        fixup(FIX_STRING, fi, 0);
        break;
    case Oxcmp:
        if (i->arg[1].k == K_INT) {
            require(fits8(i->arg[1].v), "an immediate that fits in 8 bits");
            TEXT(0, "cmpl $%d, %%%s", i->arg[1].v, reg32[i->arg[0].v]);
            byte(0x83);
            modrm(7, hw(i->arg[0]));
            byte(i->arg[1].v);
        } else {
            TEXT(0, "cmpl %%%s, %%%s", reg32[i->arg[1].v], reg32[i->arg[0].v]);
            byte(0x39);
            modrm(hw(i->arg[1]), hw(i->arg[0]));
        }
        break;
    case Ocall:
        TEXT(0, "call %s", symname[i->arg[0].v]);
        byte(0xe8);
        fixup(i->arg[0].v == S_PRINT_INT   ? FIX_PRINT_INT
              : i->arg[0].v == S_PRINT_STR ? FIX_PRINT_STR
                                           : FIX_FUNC,
              fi, i->arg[0].v);
        break;
    default: /* add, sub, mul */
        emit_alu(i);
        break;
    }
}

/* where a jump goes: through the empty blocks that only jump */
static void uffind(struct blk **pb, struct blk **uf)
{
    struct blk **p = &uf[(*pb)->id];
    if (*p) {
        uffind(p, uf);
        *pb = *p;
    }
}

static void simpljmp(struct fn *f)
{
    struct blk *uf[MAXB] = {0};
    require(f->nstore < MAXB, "more blocks");
    struct blk *ret = &f->blk[f->nstore++];
    memset(ret, 0, sizeof *ret);
    snprintf(ret->name, sizeof ret->name, "ret");
    ret->id = f->nblk;
    ret->jmp.type = Jret0;
    f->rpo[f->nblk++] = ret;
    for (int n = 0; n < f->nblk - 1; n++) {
        struct blk *b = f->rpo[n];
        if (b->jmp.type == Jret0) {
            b->jmp.type = Jjmp;
            b->s1 = ret;
        }
        if (b->nins == 0) {
            require(b->jmp.type == Jjmp, "an empty block that does not just jump");
            uffind(&b->s1, uf);
            uf[b->id] = b->s1;
        }
    }
    for (int n = 0; n < f->nblk; n++) {
        if (f->rpo[n]->s1) {
            uffind(&f->rpo[n]->s1, uf);
        }
        if (f->rpo[n]->s2) {
            uffind(&f->rpo[n]->s2, uf);
        }
    }
}

static void emit_fn(int fi)
{
    struct fn *f = &fns[fi];
    simpljmp(f);
    fnoff[fi] = clen;
    TEXT(1, "%s:", symname[f->sym]);
    TEXT(0, "pushq %%rbp");
    byte(0x55);
    TEXT(0, "movq %%rsp, %%rbp");
    byte(0x48);
    byte(0x89);
    byte(0xe5);
    int nclob = 0;
    for (int k = 0; rclob[k] >= 0; k++) {
        nclob += (f->reg >> rclob[k]) & 1;
    }
    int fsz = 8 * (nclob & 1);
    if (fsz) {
        TEXT(0, "subq $%d, %%rsp", fsz);
        byte(0x48);
        byte(0x83);
        byte(0xec);
        byte(fsz);
    }
    for (int k = 0; rclob[k] >= 0; k++) {
        if (f->reg & BIT(rclob[k])) {
            TEXT(0, "pushq %%%s", reg64[rclob[k]]);
            byte(0x50 + hw(tmpr(rclob[k])));
        }
    }
    for (int n = 0; n < f->nblk; n++) {
        struct blk *b = f->rpo[n];
        struct blk *next = n + 1 < f->nblk ? f->rpo[n + 1] : NULL;
        blkoff[fi][b->id] = clen;
        TEXT(1, ".bb%d: # @%s", b->id, b->name);
        for (int k = 0; k < b->nins; k++) {
            emit_ins(fi, &b->ins[k]);
        }
        if (b->jmp.type == Jret0) {
            for (int k = 4; k >= 0; k--) {
                if (f->reg & BIT(rclob[k])) {
                    TEXT(0, "popq %%%s", reg64[rclob[k]]);
                    byte(0x58 + hw(tmpr(rclob[k])));
                }
            }
            TEXT(0, "leave");
            byte(0xc9);
            TEXT(0, "ret");
            byte(0xc3);
        } else {
            struct blk *to = b->s1; /* the block to jump to, if it is not the next one */
            if (b->jmp.type == Jjlt) {
                /* jump on the condition to s2, and on to s1; if s2 is the block laid out next,
                 * swap them and jump on the opposite condition instead */
                int swapped = next == b->s2;
                if (swapped) {
                    b->s2 = b->s1;
                    b->s1 = next;
                }
                TEXT(0, "j%s .bb%d", swapped ? "l" : "ge", b->s2->id);
                byte(0x0f);
                byte(swapped ? 0x8c : 0x8d);
                fixup(FIX_BLOCK, fi, b->s2->id);
                to = b->s1;
            }
            if (to != next) {
                TEXT(0, "jmp .bb%d", to->id);
                byte(0xe9);
                fixup(FIX_BLOCK, fi, to->id);
            }
        }
    }
}

static void stage_emit(void)
{
    P("== stage_emit: jump threading, frame layout, two-address instructions, bytes ==\n");
    for (int fi = 0; fi < nfn; fi++) {
        emit_fn(fi);
    }
    for (int k = 0; k < nlines; k++) {
        if (lines[k].label) {
            P("%s\n", lines[k].text);
        } else {
            P("  +0x%04x  %s\n", lines[k].off, lines[k].text);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_export
 *
 * QBE leaves the assembling and linking to other programs. Here the runtime (print_int and
 * print_str) and the string follow the functions in the same buffer, the rel32 fields are
 * patched, and main is run.
 * ---------------------------------------------------------------------------------------- */

static void stage_export(const char *path)
{
    P("== stage_export: link the runtime, patch the jumps, write the code and run it ==\n");
    /* print_int(rdi): the digits are built backwards on the stack, then written to stdout */
    print_int_off = clen;
    static const uint8_t print_int[] = {
        0x48, 0x89, 0xf8, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x74, 0x24, 0x20, 0x48, 0xc7,
        0xc1, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xff, 0xce, 0xc6, 0x06, 0x0a, 0x31, 0xd2, 0x48,
        0xf7, 0xf1, 0x80, 0xc2, 0x30, 0x48, 0xff, 0xce, 0x88, 0x16, 0x48, 0x85, 0xc0, 0x75,
        0xee, 0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0x29, 0xf2, 0xb8, 0x01, 0x00, 0x00, 0x00,
        0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x20, 0xc3};
    memcpy(&code[clen], print_int, sizeof print_int);
    clen += (int)sizeof print_int;
    /* print_str(rdi): rdi points at a length byte followed by the characters */
    print_str_off = clen;
    static const uint8_t print_str[] = {0x0f, 0xb6, 0x17, 0x48, 0x8d, 0x77, 0x01, 0xb8, 0x01, 0x00,
                                        0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xc3};
    memcpy(&code[clen], print_str, sizeof print_str);
    clen += (int)sizeof print_str;
    string_off = clen;
    code[clen++] = (uint8_t)strlen(HELLO_STRING);
    memcpy(&code[clen], HELLO_STRING, strlen(HELLO_STRING));
    clen += (int)strlen(HELLO_STRING);

    for (int k = 0; k < nfix; k++) {
        int target = string_off;
        switch (fixups[k].kind) {
        case FIX_BLOCK:
            target = blkoff[fixups[k].fn][fixups[k].target];
            break;
        case FIX_FUNC:
            target = fnoff[fixups[k].target == fns[0].sym ? 0 : 1];
            break;
        case FIX_PRINT_INT:
            target = print_int_off;
            break;
        case FIX_PRINT_STR:
            target = print_str_off;
            break;
        default: /* FIX_STRING */
            break;
        }
        int rel = target - (fixups[k].at + 4);
        for (int j = 0; j < 4; j++) {
            code[fixups[k].at + j] = (uint8_t)(rel >> (8 * j));
        }
        P("+0x%04x  rel32 %d to +0x%04x\n", fixups[k].at, rel, target);
    }
    P("final code, %d bytes:", clen);
    for (int k = 0; k < clen; k++) {
        if (k % 16 == 0) {
            P("\n+0x%04x:", k);
        }
        P(" %02x", code[k]);
    }
    P("\n");

    if (path != NULL) {
        FILE *fp = fopen(path, "wb");
        // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
        if (fp == NULL || fwrite(code, 1, (size_t)clen, fp) != (size_t)clen || fclose(fp) != 0) {
            perror(path);
            exit(1);
        }
        // GCOVR_EXCL_STOP
    }
    void *buf = mmap(NULL, sizeof code, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (buf == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    memcpy(buf, code, (size_t)clen);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (mprotect(buf, sizeof code, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    /* ISO C forbids casting an object pointer to a function pointer, so copy the bits. */
    void *entry = (uint8_t *)buf + fnoff[nfn - 1];
    void (*fn)(void);
    memcpy(&fn, &entry, sizeof fn);
    fn();
}

int main(int argc, char **argv)
{
    const struct program *program = NULL;
    if (argc < 2 || (program = find_program(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    program->build();
    stage_il();
    stage_ssa();
    stage_abi();
    stage_isel();
    stage_live();
    stage_spill();
    stage_rega();
    stage_emit();
    stage_export(argc > 2 ? argv[2] : NULL);
    return 0;
}
