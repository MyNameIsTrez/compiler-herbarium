/*
 * clang: a cut-down version of the Clang and LLVM pipeline for x86-64.
 *
 * Clang emits "-O0 style" SSA IR in which every local variable lives in memory (an alloca with
 * loads and stores). LLVM then promotes those slots to SSA values (mem2reg), selects x86
 * instructions block by block with SelectionDAG, removes the PHIs, and allocates registers
 * on live intervals with the basic allocator (RABasic).
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout, the intermediate form after each stage on stderr,
 *   and writes the machine code to code.bin if a path is given.
 *
 * Stages, in the order of the real pipeline:
 *   stage_irgen               clang's CodeGen: IR with allocas, loads, stores and named blocks
 *   stage_mem2reg             PromoteMemToReg: dominator tree, dominance frontier, PHI placement
 *   stage_isel                SelectionDAGISel: a DAG per block, pattern selection, scheduling
 *   stage_phi_elim            PHIElimination: PHIs become copies at the end of the predecessors
 *   stage_two_address         TwoAddressInstruction: A = B op C becomes A = B; A op= C
 *   stage_live_intervals      LiveIntervals: slot indexes and live segments per virtual register
 *   stage_coalesce            RegisterCoalescer: join copies whose intervals do not overlap
 *   stage_regalloc            RABasic: priority queue, hints, interference, call clobbers
 *   stage_prologue_epilogue   push and pop the callee-saved registers that were used
 *   stage_encode              emit bytes
 *   stage_export              link the functions in memory and run main
 */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

#define MAX_FUNS 2
#define MAX_BLOCKS 6
#define MAX_INSTS 16
#define MAX_MI 40
#define MAX_VALS 24
#define MAX_SEG 6
#define HELLO_STRING "Hello, world!\n"
#define NATIVE_PRINT_INT (-1)
#define NATIVE_PRINT_STR (-2)

/* ------------------------------------------------------------------------------------------
 * Values and instructions. One instruction type serves the IR and the machine IR.
 * ---------------------------------------------------------------------------------------- */

enum { V_NONE, V_REG, V_IMM, V_BLOCK, V_FUN, V_STR, V_PHYS };
struct val {
    int kind;
    int64_t n;
};

/* The order of I_ADD to I_SLT matches N_ADD to N_SETLT below. */
enum {
    I_ALLOCA, I_LOAD, I_STORE, I_ADD, I_SUB, I_MUL, I_SLT, I_BR, I_CONDBR, I_CALL, I_RET, I_PHI,
    I_DEAD,
    M_MOV64ri32, M_LEA64r, M_ADD64rr, M_ADD64ri8, M_SUB64ri8, M_IMUL64rr, M_CMP64ri8, M_JCC,
    M_JMP, M_CALL, M_RET, M_COPY, M_PHI, M_PUSH, M_POP,
};
static const char *const mop_names[] = {
    "MOV64ri32", "LEA64r", "ADD64rr", "ADD64ri8", "SUB64ri8", "IMUL64rr", "CMP64ri8", "JCC_1",
    "JMP_1", "CALL64pcrel32", "RET64", "COPY", "PHI", "PUSH64r", "POP64r",
};

struct inst {
    int op;
    struct val d, a, b, c; /* IR: result, operands. Machine IR: def, two uses, implicit def */
    struct val in[2];      /* PHI incoming values and their predecessor blocks */
    int inb[2];
    int nin;
    int slot; /* slot index of the use point, set by live intervals */
};

struct interval {
    int nseg;
    int s[MAX_SEG], e[MAX_SEG];
    int refs, weight, phys;
};

struct blk {
    char name[16];
    struct inst code[MAX_INSTS]; /* IR */
    int n;
    struct inst m[MAX_MI]; /* machine IR */
    int mn;
    int nphi;
    int succ[2], nsucc;
    int pred[4], npred;
    int idom, rpo;
    unsigned char df[MAX_BLOCKS];
    unsigned char live_in[MAX_VALS], live_out[MAX_VALS];
    int first_slot;
};

struct fun {
    const char *name;
    int has_param, has_result;
    struct blk b[MAX_BLOCKS];
    int nb;
    char vname[MAX_VALS][32];
    int num[MAX_VALS];
    int nv;
    int nvregs;
    struct interval ivl[MAX_VALS];
    struct interval fixed[16];
    int call_slot[4], ncall;
    int uses_rbx;
};

static struct fun funs[MAX_FUNS];
static int nfun;

enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSI = 6, RDI = 7, R8 = 8, R9 = 9, R10 = 10, R11 = 11 };
static const char *const reg_names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
/* The order of GR64 in X86RegisterInfo.td, up to the first callee-saved register. */
static const int alloc_order[] = {RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, RBX};
#define NUM_ORDER ((int)(sizeof alloc_order / sizeof alloc_order[0]))

static struct val vr(int64_t n) { return (struct val){V_REG, n}; }
static struct val imm(int64_t n) { return (struct val){V_IMM, n}; }
static struct val bv(int64_t n) { return (struct val){V_BLOCK, n}; }
static struct val fv(int64_t n) { return (struct val){V_FUN, n}; }
static struct val ph(int64_t n) { return (struct val){V_PHYS, n}; }
static const struct val NONE = {V_NONE, 0};

static int same(struct val a, struct val b) { return (a.kind == b.kind) & (a.n == b.n); }

static const char *fun_name(int64_t n)
{
    return n == NATIVE_PRINT_INT ? "print_int" : n == NATIVE_PRINT_STR ? "print_str" : funs[n].name;
}

/* ------------------------------------------------------------------------------------------
 * stage_irgen
 *
 * Clang's CodeGen walks the AST and emits IR with an IRBuilder. A local variable becomes an
 * alloca in the entry block, every read a load and every write a store. The blocks are named
 * as CGStmt.cpp names them (if.then, if.else, if.end, while.cond, while.body, while.end), and
 * a function with several returns jumps to one block called return, which loads "retval". The
 * IRBuilder folds a binary operation on two constants. The examples are hardcoded as the calls
 * the AST walk would make.
 * ---------------------------------------------------------------------------------------- */

static struct fun *F;
static int cur;

static struct inst *add_inst(int op)
{
    struct blk *b = &F->b[cur];
    b->code[b->n] = (struct inst){.op = op};
    return &b->code[b->n++];
}

static struct val new_val(const char *name)
{
    snprintf(F->vname[F->nv], sizeof F->vname[0], "%s", name);
    return vr(F->nv++);
}

static int block(const char *name)
{
    snprintf(F->b[F->nb].name, sizeof F->b[0].name, "%s", name);
    return F->nb++;
}

static void fn_begin(const char *name, int has_param)
{
    F = &funs[nfun++];
    F->name = name;
    F->has_param = has_param;
    if (has_param) {
        new_val("n");
    }
    cur = block("entry");
}

static struct val alloca_(const char *name)
{
    struct inst *i = add_inst(I_ALLOCA);
    i->d = new_val(name);
    return i->d;
}

static struct val load(struct val ptr)
{
    struct inst *i = add_inst(I_LOAD);
    i->a = ptr;
    i->d = new_val("");
    return i->d;
}

static void store(struct val value, struct val ptr)
{
    struct inst *i = add_inst(I_STORE);
    i->a = value;
    i->b = ptr;
}

static struct val bin(int op, struct val a, struct val b, const char *name)
{
    if ((a.kind == V_IMM) & (b.kind == V_IMM)) { /* IRBuilder's ConstantFolder */
        return imm(op == I_ADD ? a.n + b.n : op == I_SUB ? a.n - b.n : a.n * b.n);
    }
    struct inst *i = add_inst(op);
    i->a = a;
    i->b = b;
    i->d = new_val(name);
    return i->d;
}

static struct val slt(struct val a, struct val b, const char *name)
{
    struct inst *i = add_inst(I_SLT);
    i->a = a;
    i->b = b;
    i->d = new_val(name);
    return i->d;
}

static struct val call(int64_t fun, struct val arg, int has_result, const char *name)
{
    struct inst *i = add_inst(I_CALL);
    i->a = fv(fun);
    i->b = arg;
    if (has_result) {
        i->d = new_val(name);
    }
    return i->d;
}

static void br(int target) { add_inst(I_BR)->a = bv(target); }

static void condbr(struct val cond, int yes, int no)
{
    struct inst *i = add_inst(I_CONDBR);
    i->a = cond;
    i->b = bv(yes);
    i->c = bv(no);
}

static void ret(struct val value)
{
    add_inst(I_RET)->a = value;
    F->has_result |= value.kind != V_NONE;
}

static void build_hello(void)
{
    fn_begin("main", 0);
    call(NATIVE_PRINT_STR, (struct val){V_STR, 0}, 0, "");
    ret(NONE);
}

static void build_arithmetic(void)
{
    fn_begin("main", 0);
    struct val sum = bin(I_ADD, imm(2), imm(3), "add");
    struct val product = bin(I_MUL, sum, imm(4), "mul");
    call(NATIVE_PRINT_INT, bin(I_SUB, product, imm(6), "sub"), 0, "");
    ret(NONE);
}

static void build_branch(void)
{
    fn_begin("main", 0);
    const int then_ = block("if.then"), else_ = block("if.else"), end = block("if.end");
    const struct val a = alloca_("a"), b = alloca_("b");
    store(imm(3), a);
    store(imm(5), b);
    const struct val x = load(a);
    const struct val y = load(b);
    condbr(slt(x, y, "cmp"), then_, else_);
    cur = then_;
    call(NATIVE_PRINT_INT, load(b), 0, "");
    br(end);
    cur = else_;
    call(NATIVE_PRINT_INT, load(a), 0, "");
    br(end);
    cur = end;
    ret(NONE);
}

static void build_loop(void)
{
    fn_begin("main", 0);
    const int cond = block("while.cond"), body = block("while.body"), end = block("while.end");
    const struct val sum = alloca_("sum"), i = alloca_("i");
    store(imm(0), sum);
    store(imm(1), i);
    br(cond);
    cur = cond;
    const struct val i0 = load(i);
    condbr(slt(i0, imm(11), "cmp"), body, end);
    cur = body;
    const struct val s1 = load(sum);
    const struct val i1 = load(i);
    store(bin(I_ADD, s1, i1, "add"), sum);
    const struct val i2 = load(i);
    store(bin(I_ADD, i2, imm(1), "add1"), i);
    br(cond);
    cur = end;
    call(NATIVE_PRINT_INT, load(sum), 0, "");
    ret(NONE);
}

static void build_call(void)
{
    fn_begin("fact", 1);
    const int then_ = block("if.then"), end = block("if.end"), ret_ = block("return");
    const struct val retval = alloca_("retval"), n_addr = alloca_("n.addr");
    store(vr(0), n_addr);
    const struct val l0 = load(n_addr);
    condbr(slt(l0, imm(2), "cmp"), then_, end);
    cur = then_;
    store(imm(1), retval);
    br(ret_);
    cur = end;
    const struct val l1 = load(n_addr);
    const struct val l2 = load(n_addr);
    const struct val sub = bin(I_SUB, l2, imm(1), "sub");
    const struct val rec = call(0, sub, 1, "call");
    store(bin(I_MUL, l1, rec, "mul"), retval);
    br(ret_);
    cur = ret_;
    ret(load(retval));

    fn_begin("main", 0);
    const struct val result = call(0, imm(5), 1, "call");
    call(NATIVE_PRINT_INT, result, 0, "");
    ret(NONE);
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

static void pir(const struct fun *f, struct val v)
{
    switch (v.kind) {
    case V_REG:
        if (f->vname[v.n][0] != '\0') {
            P("%%%s", f->vname[v.n]);
        } else {
            P("%%%d", f->num[v.n]);
        }
        break;
    case V_IMM:
        P("%lld", (long long)v.n);
        break;
    case V_BLOCK:
        P("%%%s", f->b[v.n].name);
        break;
    default: /* V_STR */
        P("@.str");
        break;
    }
}

static void pir_typed(const struct fun *f, struct val v)
{
    P("%s", v.kind == V_STR ? "ptr " : "i64 ");
    pir(f, v);
}

static void print_ir_inst(const struct fun *f, const struct inst *in)
{
    P("  ");
    if (in->d.kind == V_REG) {
        pir(f, in->d);
        P(" = ");
    }
    switch (in->op) {
    case I_ALLOCA:
        P("alloca i64");
        break;
    case I_LOAD:
        P("load i64, ptr ");
        pir(f, in->a);
        break;
    case I_STORE:
        P("store ");
        pir_typed(f, in->a);
        P(", ptr ");
        pir(f, in->b);
        break;
    case I_ADD:
    case I_SUB:
    case I_MUL:
        P("%s i64 ", in->op == I_ADD ? "add" : in->op == I_SUB ? "sub" : "mul");
        pir(f, in->a);
        P(", ");
        pir(f, in->b);
        break;
    case I_SLT:
        P("icmp slt i64 ");
        pir(f, in->a);
        P(", ");
        pir(f, in->b);
        break;
    case I_BR:
        P("br label ");
        pir(f, in->a);
        break;
    case I_CONDBR:
        P("br i1 ");
        pir(f, in->a);
        P(", label ");
        pir(f, in->b);
        P(", label ");
        pir(f, in->c);
        break;
    case I_CALL:
        P("call %s @%s(", in->d.kind == V_REG ? "i64" : "void", fun_name(in->a.n));
        pir_typed(f, in->b);
        P(")");
        break;
    case I_RET:
        if (in->a.kind == V_NONE) {
            P("ret void");
        } else {
            P("ret ");
            pir_typed(f, in->a);
        }
        break;
    default: /* I_PHI */
        P("phi i64 ");
        for (int j = 0; j < in->nin; j++) {
            P("%s[ ", j == 0 ? "" : ", ");
            pir(f, in->in[j]);
            P(", %%%s ]", f->b[in->inb[j]].name);
        }
        break;
    }
    P("\n");
}

static void print_ir(const char *title)
{
    P("== %s ==\n", title);
    for (int fi = 0; fi < nfun; fi++) {
        struct fun *f = &funs[fi];
        int next = 0;
        for (int bi = 0; bi < f->nb; bi++) {
            for (int i = 0; i < f->b[bi].n; i++) {
                const struct inst *in = &f->b[bi].code[i];
                if ((in->d.kind == V_REG) & (f->vname[in->d.n][0] == '\0')) {
                    f->num[in->d.n] = next++;
                }
            }
        }
        P("define %s @%s(%s) {\n", f->has_result ? "i64" : "void", f->name,
          f->has_param ? "i64 %n" : "");
        for (int bi = 0; bi < f->nb; bi++) {
            P("%s:\n", f->b[bi].name);
            for (int i = 0; i < f->b[bi].n; i++) {
                print_ir_inst(f, &f->b[bi].code[i]);
            }
        }
        P("}\n");
    }
}

static void stage_irgen(const struct program *program)
{
    program->build();
    print_ir("stage_irgen: IR as clang emits it, every local in an alloca");
}

/* ------------------------------------------------------------------------------------------
 * stage_mem2reg
 *
 * PromoteMemToReg. An alloca with a single store is rewritten in place: each load becomes the
 * stored value. Any other alloca gets PHIs at the iterated dominance frontier of its stores,
 * but only in blocks where it is live-in, and a walk down the dominator tree renames the loads
 * and stores.
 * ---------------------------------------------------------------------------------------- */

static void compute_cfg(struct fun *f)
{
    for (int i = 0; i < f->nb; i++) {
        struct blk *b = &f->b[i];
        const struct inst *t = &b->code[b->n - 1];
        b->nsucc = 0;
        b->npred = 0;
        if (t->op == I_BR) {
            b->succ[b->nsucc++] = (int)t->a.n;
        }
        if (t->op == I_CONDBR) {
            b->succ[b->nsucc++] = (int)t->b.n;
            b->succ[b->nsucc++] = (int)t->c.n;
        }
    }
    for (int i = 0; i < f->nb; i++) {
        for (int s = 0; s < f->b[i].nsucc; s++) {
            struct blk *to = &f->b[f->b[i].succ[s]];
            to->pred[to->npred++] = i;
        }
    }
}

static int po[MAX_BLOCKS], npo;
static unsigned char seen[MAX_BLOCKS];

static void dfs(const struct fun *f, int b)
{
    seen[b] = 1;
    for (int s = 0; s < f->b[b].nsucc; s++) {
        if (!seen[f->b[b].succ[s]]) {
            dfs(f, f->b[b].succ[s]);
        }
    }
    po[npo++] = b;
}

static int intersect(const struct fun *f, int a, int b)
{
    while (a != b) {
        if (f->b[a].rpo > f->b[b].rpo) {
            a = f->b[a].idom;
        } else {
            b = f->b[b].idom;
        }
    }
    return a;
}

/* The dominator tree by the iterative algorithm over the reverse postorder. */
static void compute_dom(struct fun *f)
{
    memset(seen, 0, sizeof seen);
    npo = 0;
    dfs(f, 0);
    for (int i = 0; i < npo; i++) {
        f->b[po[i]].rpo = npo - 1 - i;
    }
    for (int i = 0; i < f->nb; i++) {
        f->b[i].idom = -1;
    }
    f->b[0].idom = 0;
    int changed;
    do {
        changed = 0;
        for (int i = npo - 2; i >= 0; i--) {
            struct blk *b = &f->b[po[i]];
            int idom = -1;
            for (int p = 0; p < b->npred; p++) {
                if (f->b[b->pred[p]].idom >= 0) {
                    idom = idom < 0 ? b->pred[p] : intersect(f, b->pred[p], idom);
                }
            }
            if (idom != b->idom) {
                b->idom = idom;
                changed = 1;
            }
        }
    } while (changed);
}

static void compute_df(struct fun *f)
{
    for (int i = 0; i < f->nb; i++) {
        for (int p = 0; p < f->b[i].npred; p++) {
            int runner = f->b[i].pred[p];
            while (runner != f->b[i].idom) {
                f->b[runner].df[i] = 1;
                runner = f->b[runner].idom;
            }
        }
    }
}

static int dominates(const struct fun *f, int a, int b)
{
    while ((b != a) & (b != 0)) {
        b = f->b[b].idom;
    }
    return b == a;
}

static void replace_uses(struct fun *f, struct val from, struct val to)
{
    for (int bi = 0; bi < f->nb; bi++) {
        for (int i = 0; i < f->b[bi].n; i++) {
            struct inst *in = &f->b[bi].code[i];
            struct val *ops[] = {&in->a, &in->b, &in->c, &in->in[0], &in->in[1]};
            for (int k = 0; k < 5; k++) {
                if ((ops[k]->kind == V_REG) & (ops[k]->n == from.n)) {
                    *ops[k] = to;
                }
            }
        }
    }
}

static void pset(const struct fun *f, const unsigned char *set)
{
    for (int i = 0; i < f->nb; i++) {
        if (set[i]) {
            P(" %s", f->b[i].name);
        }
    }
}

#define MAX_AL 4

static struct val rename_cur[MAX_AL];

static void rename_block(struct fun *f, int bi, const int *promoted)
{
    struct val cur_in[MAX_AL];
    memcpy(cur_in, rename_cur, sizeof cur_in);
    struct blk *b = &f->b[bi];
    for (int i = 0; i < b->n; i++) {
        struct inst *in = &b->code[i];
        if (in->op == I_PHI) {
            rename_cur[in->c.n] = in->d;
        } else if (in->op == I_LOAD) {
            replace_uses(f, in->d, rename_cur[promoted[in->a.n]]);
            in->op = I_DEAD;
        } else if (in->op == I_STORE) {
            rename_cur[promoted[in->b.n]] = in->a;
            in->op = I_DEAD;
        }
    }
    for (int s = 0; s < b->nsucc; s++) {
        struct blk *to = &f->b[b->succ[s]];
        for (int i = 0; i < to->nphi; i++) {
            struct inst *phi = &to->code[i];
            phi->in[phi->nin] = rename_cur[phi->c.n];
            phi->inb[phi->nin++] = bi;
        }
    }
    struct val here[MAX_AL];
    memcpy(here, rename_cur, sizeof here);
    for (int c = 0; c < f->nb; c++) {
        if ((c != bi) & (f->b[c].idom == bi)) {
            memcpy(rename_cur, here, sizeof here);
            rename_block(f, c, promoted);
        }
    }
    memcpy(rename_cur, cur_in, sizeof cur_in);
}

static void compact(struct fun *f)
{
    for (int bi = 0; bi < f->nb; bi++) {
        struct blk *b = &f->b[bi];
        int w = 0;
        for (int i = 0; i < b->n; i++) {
            if (b->code[i].op != I_DEAD) {
                b->code[w++] = b->code[i];
            }
        }
        b->n = w;
    }
}

static void promote_function(struct fun *f)
{
    compute_cfg(f);
    compute_dom(f);
    compute_df(f);
    P("%s: immediate dominators:", f->name);
    for (int i = 0; i < f->nb; i++) {
        P(" %s<-%s", f->b[i].name, f->b[f->b[i].idom].name);
    }
    P("\n");

    int promoted[MAX_VALS], al[MAX_AL], nal = 0, version[MAX_AL] = {0};
    memset(promoted, -1, sizeof promoted);
    for (int i = 0; i < f->b[0].n; i++) {
        if (f->b[0].code[i].op == I_ALLOCA) {
            al[nal] = (int)f->b[0].code[i].d.n;
            promoted[al[nal]] = nal;
            nal++;
        }
    }

    for (int k = 0; k < nal; k++) {
        const struct val slot = vr(al[k]);
        int stores = 0, store_block = -1, store_pos = -1, ok = 1;
        unsigned char defs[MAX_BLOCKS] = {0};
        for (int bi = 0; bi < f->nb; bi++) {
            for (int i = 0; i < f->b[bi].n; i++) {
                const struct inst *in = &f->b[bi].code[i];
                if ((in->op == I_STORE) & same(in->b, slot)) {
                    stores++;
                    defs[bi] = 1;
                    store_block = bi;
                    store_pos = i;
                }
            }
        }
        for (int bi = 0; bi < f->nb; bi++) {
            for (int i = 0; i < f->b[bi].n; i++) {
                const struct inst *in = &f->b[bi].code[i];
                if ((in->op == I_LOAD) & same(in->a, slot)) {
                    ok &= bi == store_block ? i > store_pos : dominates(f, store_block, bi);
                }
            }
        }
        if ((stores == 1) & ok) {
            /* rewriteSingleStoreAlloca: every load sees the one stored value. */
            const struct val stored = f->b[store_block].code[store_pos].a;
            for (int bi = 0; bi < f->nb; bi++) {
                for (int i = 0; i < f->b[bi].n; i++) {
                    struct inst *in = &f->b[bi].code[i];
                    if ((in->op == I_LOAD) & same(in->a, slot)) {
                        replace_uses(f, in->d, stored);
                        in->op = I_DEAD;
                    }
                }
            }
            f->b[store_block].code[store_pos].op = I_DEAD;
            P("%%%s: one store, loads replaced by the stored value\n", f->vname[al[k]]);
            continue;
        }

        /* ComputeLiveInBlocks: live-in where a load comes before any store, and in the
         * predecessors that do not store themselves. */
        unsigned char live[MAX_BLOCKS] = {0};
        for (int bi = 0; bi < f->nb; bi++) {
            int first = 0;
            for (int i = 0; i < f->b[bi].n; i++) {
                const struct inst *in = &f->b[bi].code[i];
                first = first ? first : ((in->op == I_LOAD) & same(in->a, slot)) ? 1
                        : ((in->op == I_STORE) & same(in->b, slot))               ? 2
                                                                                  : 0;
            }
            live[bi] = first == 1;
        }
        int changed;
        do {
            changed = 0;
            for (int bi = 0; bi < f->nb; bi++) {
                for (int p = 0; p < f->b[bi].npred; p++) {
                    const int pr = f->b[bi].pred[p];
                    if (live[bi] & !defs[pr] & !live[pr]) {
                        live[pr] = 1;
                        changed = 1;
                    }
                }
            }
        } while (changed);

        /* The iterated dominance frontier of the blocks that store. */
        unsigned char idf[MAX_BLOCKS] = {0}, work[MAX_BLOCKS];
        memcpy(work, defs, sizeof work);
        do {
            changed = 0;
            for (int x = 0; x < f->nb; x++) {
                for (int y = 0; y < f->nb; y++) {
                    if (work[x] & f->b[x].df[y] & !idf[y]) {
                        idf[y] = 1;
                        work[y] = 1;
                        changed = 1;
                    }
                }
            }
        } while (changed);

        P("%%%s: stores in:", f->vname[al[k]]);
        pset(f, defs);
        P("; frontier:");
        pset(f, idf);
        P("; live-in:");
        pset(f, live);
        P("; phis in:");
        for (int bi = 0; bi < f->nb; bi++) {
            if (idf[bi] & live[bi]) {
                struct blk *b = &f->b[bi];
                struct inst phi = {.op = I_PHI, .c = imm(k)};
                F = f;
                char name[32];
                snprintf(name, sizeof name, "%.20s.%d", f->vname[al[k]], version[k]++);
                phi.d = new_val(name);
                memmove(&b->code[b->nphi + 1], &b->code[b->nphi],
                        (size_t)(b->n - b->nphi) * sizeof b->code[0]);
                b->code[b->nphi++] = phi;
                b->n++;
                P(" %s", b->name);
            }
        }
        P("\n");
    }

    for (int k = 0; k < MAX_AL; k++) {
        rename_cur[k] = imm(0); /* an undefined value reads as zero */
    }
    rename_block(f, 0, promoted);
    for (int i = 0; i < f->b[0].n; i++) {
        struct inst *in = &f->b[0].code[i];
        if (in->op == I_ALLOCA) {
            in->op = I_DEAD;
        }
    }
    compact(f);
}

static void stage_mem2reg(void)
{
    P("== stage_mem2reg: what it did ==\n");
    for (int fi = 0; fi < nfun; fi++) {
        promote_function(&funs[fi]);
    }
    print_ir("stage_mem2reg: IR after promotion to SSA");
}

/* ------------------------------------------------------------------------------------------
 * Machine IR printing, shared by every stage from stage_isel on.
 * ---------------------------------------------------------------------------------------- */

static void pm(struct val v)
{
    switch (v.kind) {
    case V_REG:
        P("%%%lld", (long long)v.n);
        break;
    case V_PHYS:
        P("$%s", reg_names[v.n]);
        break;
    case V_IMM:
        P("%lld", (long long)v.n);
        break;
    case V_BLOCK:
        P("%%bb.%lld", (long long)v.n);
        break;
    case V_FUN:
        P("@%s", fun_name(v.n));
        break;
    default: /* V_STR */
        P("@.str");
        break;
    }
}

static void print_minst(const struct inst *in)
{
    if (in->d.kind != V_NONE) {
        pm(in->d);
        P(" = ");
    }
    P("%s", mop_names[in->op - M_MOV64ri32]);
    switch (in->op) {
    case M_PHI:
        for (int j = 0; j < in->nin; j++) {
            P("%s ", j == 0 ? "" : ",");
            pm(in->in[j]);
            P(", %%bb.%d", in->inb[j]);
        }
        break;
    case M_CALL:
        P(" ");
        pm(in->a);
        P(", implicit ");
        pm(in->b);
        if (in->c.kind != V_NONE) {
            P(", implicit-def ");
            pm(in->c);
        }
        break;
    case M_RET:
        if (in->b.kind != V_NONE) {
            P(" implicit ");
            pm(in->b);
        }
        break;
    case M_JCC:
        P(" ");
        pm(in->a);
        P(", 12, implicit $eflags");
        break;
    case M_CMP64ri8:
        P(" ");
        pm(in->a);
        P(", ");
        pm(in->b);
        P(", implicit-def $eflags");
        break;
    default:
        if (in->a.kind != V_NONE) {
            P(" ");
            pm(in->a);
        }
        if (in->b.kind != V_NONE) {
            P(", ");
            pm(in->b);
        }
        break;
    }
    P("\n");
}

static void print_mir(const char *title, int with_slots)
{
    P("== %s ==\n", title);
    for (int fi = 0; fi < nfun; fi++) {
        const struct fun *f = &funs[fi];
        P("%s:\n", f->name);
        for (int bi = 0; bi < f->nb; bi++) {
            P("bb.%d.%s:\n", bi, f->b[bi].name);
            for (int i = 0; i < f->b[bi].mn; i++) {
                if (with_slots) {
                    P("  %3d  ", f->b[bi].m[i].slot);
                } else {
                    P("  ");
                }
                print_minst(&f->b[bi].m[i]);
            }
        }
    }
}

static struct blk *MB;
static struct fun *MF;

static struct inst *mi(int op, struct val d, struct val a, struct val b)
{
    struct inst *in = &MB->m[MB->mn++];
    *in = (struct inst){.op = op, .d = d, .a = a, .b = b};
    return in;
}

static void minsert(struct blk *b, int pos, struct inst in)
{
    memmove(&b->m[pos + 1], &b->m[pos], (size_t)(b->mn - pos) * sizeof b->m[0]);
    b->m[pos] = in;
    b->mn++;
}

static int is_term(int op) { return (op == M_JCC) | (op == M_JMP) | (op == M_RET); }

static int first_terminator(const struct blk *b)
{
    int i = 0; /* every block ends in a terminator */
    while (!is_term(b->m[i].op)) {
        i++;
    }
    return i;
}

static int new_vreg(void) { return MF->nvregs++; }

/* Only the patterns that the examples need are written. A shape that none of them contains
 * stops the program here. */
static void require(int ok, const char *what)
{
    // GCOVR_EXCL_START: every shape in the examples has a pattern
    if (!ok) {
        fprintf(stderr, "no pattern for %s\n", what);
        exit(1);
    }
    // GCOVR_EXCL_STOP
}

/* ------------------------------------------------------------------------------------------
 * stage_isel
 *
 * SelectionDAG, one basic block at a time. A value that another block uses (or a PHI takes)
 * lives in a virtual register; everything else is a node of the block's DAG. Equal pure nodes
 * are shared. Side effects (copies to registers, calls, branches) are linked by a chain, which
 * fixes their order. Selection picks the x86 instruction for each node, and the scheduler
 * emits the nodes in an order that follows the chain and puts operands first.
 * ---------------------------------------------------------------------------------------- */

enum { N_CONST, N_STR, N_REG, N_ADD, N_SUB, N_MUL, N_SETLT, N_COPYTO, N_CALL, N_BRCOND, N_BR,
       N_RET };
static const char *const dop_names[] = {"Constant", "GlobalAddress", "CopyFromReg", "add", "sub",
                                        "mul", "setcc", "CopyToReg", "call", "brcond", "br",
                                        "ret"};
#define MAX_DAG 40

struct dnode {
    int op, a, b, chain;
    struct val v;
    int sel;          /* the machine opcode, or -1 */
    struct val dest;  /* the register this node's result is selected into, if one is known */
    struct val res;   /* the register holding the result once emitted */
    int done;
    int has_res;
};

static struct dnode dag[MAX_DAG];
static int ndag;
static int node_of[MAX_VALS];
static int vreg_of[MAX_VALS];

static int mk(int op, int a, int b, int chain, struct val v)
{
    if ((chain < 0) & (op <= N_SETLT)) {
        for (int i = 0; i < ndag; i++) {
            const struct dnode *d = &dag[i];
            if ((d->op == op) & (d->a == a) & (d->b == b) & (d->chain == chain) & same(d->v, v)) {
                return i;
            }
        }
    }
    dag[ndag] = (struct dnode){.op = op, .a = a, .b = b, .chain = chain, .v = v, .sel = -1};
    return ndag++;
}

static int copy_to(int chain, struct val reg, int src)
{
    struct dnode *s = &dag[src];
    /* A constant is selected straight into the register. So is a computed value that goes to
     * a virtual register, which is what the register coalescer would make of a copy. */
    if ((s->dest.kind == V_NONE) & ((s->op != N_REG) | (s->v.kind == V_PHYS)) &
        ((reg.kind == V_REG) | (s->op == N_CONST))) {
        s->dest = reg;
    }
    return mk(N_COPYTO, src, -1, chain, reg);
}

static int operand(struct val v)
{
    if (v.kind == V_IMM) {
        return mk(N_CONST, -1, -1, -1, v);
    }
    if (v.kind == V_STR) {
        return mk(N_STR, -1, -1, -1, v);
    }
    return node_of[v.n] >= 0 ? node_of[v.n] : mk(N_REG, -1, -1, -1, vr(vreg_of[v.n]));
}

static void assign_vregs(struct fun *f)
{
    int def_block[MAX_VALS] = {0};
    memset(vreg_of, -1, sizeof vreg_of);
    f->nvregs = 0;
    if (f->has_param) {
        vreg_of[0] = f->nvregs++;
    }
    for (int bi = 0; bi < f->nb; bi++) {
        for (int i = 0; i < f->b[bi].n; i++) {
            const struct inst *in = &f->b[bi].code[i];
            if (in->d.kind == V_REG) {
                def_block[in->d.n] = bi;
                if (in->op == I_PHI) {
                    vreg_of[in->d.n] = f->nvregs++;
                }
            }
        }
    }
    for (int bi = 0; bi < f->nb; bi++) {
        for (int i = 0; i < f->b[bi].n; i++) {
            const struct inst *in = &f->b[bi].code[i];
            const struct val *ops[] = {&in->a, &in->b, &in->c, &in->in[0], &in->in[1]};
            for (int k = 0; k < 5; k++) {
                if (ops[k]->kind != V_REG) {
                    continue;
                }
                const int outside = (def_block[ops[k]->n] != bi) | (in->op == I_PHI);
                if (outside & (vreg_of[ops[k]->n] < 0)) {
                    vreg_of[ops[k]->n] = f->nvregs++;
                }
            }
        }
    }
}

static void print_dag(const char *title, int selected)
{
    P("  -- %s --\n", title);
    for (int i = 0; i < ndag; i++) {
        const struct dnode *d = &dag[i];
        const int machine = selected & (d->sel >= 0);
        P("  t%d: %s = %s", i, d->op >= N_COPYTO ? "ch" : d->op == N_SETLT ? "i1" : "i64",
          machine ? mop_names[d->sel - M_MOV64ri32] : dop_names[d->op]);
        const char *sep = " ";
        if (d->chain >= 0) {
            P("%st%d", sep, d->chain);
            sep = ", ";
        }
        if (d->a >= 0) {
            P("%st%d", sep, d->a);
            sep = ", ";
        }
        if (d->b >= 0) {
            P("%st%d", sep, d->b);
            sep = ", ";
        }
        if (d->v.kind != V_NONE) {
            P("%s", sep);
            if (d->op == N_CONST) {
                P("<%lld>", (long long)d->v.n);
            } else {
                pm(d->v);
            }
        }
        if (d->dest.kind != V_NONE) {
            P("   ; result goes to ");
            pm(d->dest);
        }
        P("\n");
    }
}

static void select_nodes(void)
{
    for (int i = 0; i < ndag; i++) {
        struct dnode *d = &dag[i];
        const int rhs_const = d->b >= 0 && dag[d->b].op == N_CONST;
        switch (d->op) {
        case N_CONST:
            d->sel = M_MOV64ri32;
            break;
        case N_STR:
            d->sel = M_LEA64r;
            break;
        case N_REG:
            d->sel = d->v.kind == V_PHYS ? M_COPY : -1;
            break;
        case N_ADD:
            d->sel = rhs_const ? M_ADD64ri8 : M_ADD64rr;
            break;
        case N_SUB:
            require(rhs_const, "sub with a register on the right");
            d->sel = M_SUB64ri8;
            break;
        case N_MUL:
            d->sel = M_IMUL64rr;
            break;
        case N_SETLT:
            require(rhs_const, "compare with a register on the right");
            d->sel = M_CMP64ri8;
            break;
        case N_COPYTO:
            d->sel = M_COPY;
            break;
        case N_CALL:
            d->sel = M_CALL;
            break;
        case N_BRCOND:
            d->sel = M_JCC;
            break;
        case N_BR:
            d->sel = M_JMP;
            break;
        default: /* N_RET */
            d->sel = M_RET;
            break;
        }
        if (rhs_const) {
            require((dag[d->b].v.n >= -128) & (dag[d->b].v.n <= 127), "an immediate over 8 bits");
        }
    }
}

static struct val result_reg(const struct dnode *d)
{
    return d->dest.kind != V_NONE ? d->dest : vr(new_vreg());
}

/* The scheduler: the chain first, then the operands, then the node itself. */
static struct val emit(int n)
{
    if (n < 0) {
        return NONE;
    }
    struct dnode *d = &dag[n];
    if (d->done) {
        return d->res;
    }
    d->done = 1;
    emit(d->chain);
    switch (d->op) {
    case N_CONST:
        d->res = result_reg(d);
        mi(d->sel, d->res, imm(d->v.n), NONE);
        break;
    case N_STR:
        d->res = result_reg(d);
        mi(d->sel, d->res, d->v, NONE);
        break;
    case N_REG:
        d->res = d->v;
        if (d->v.kind == V_PHYS) {
            d->res = result_reg(d);
            mi(M_COPY, d->res, d->v, NONE);
        }
        break;
    case N_ADD:
    case N_SUB:
    case N_MUL: {
        const struct val x = emit(d->a);
        const int imm_form = (dag[d->b].op == N_CONST) & (d->sel != M_IMUL64rr);
        const struct val y = imm_form ? imm(dag[d->b].v.n) : emit(d->b);
        d->res = result_reg(d);
        mi(d->sel, d->res, x, y);
        break;
    }
    case N_SETLT: {
        const struct val x = emit(d->a);
        mi(d->sel, NONE, x, imm(dag[d->b].v.n));
        break;
    }
    case N_COPYTO: {
        const struct val s = emit(d->a);
        if (!same(s, d->v)) {
            mi(M_COPY, d->v, s, NONE);
        }
        break;
    }
    case N_CALL: {
        struct inst *in = mi(M_CALL, NONE, d->v, ph(RDI)); /* the argument */
        in->c = d->has_res ? ph(RAX) : NONE;
        break;
    }
    case N_BRCOND:
        emit(d->a);
        mi(M_JCC, NONE, d->v, NONE);
        break;
    case N_BR:
        mi(M_JMP, NONE, d->v, NONE);
        break;
    default: /* N_RET */
        mi(M_RET, NONE, NONE, d->v);
        break;
    }
    return d->res;
}

struct export_copy {
    int vreg, node;
};

static void isel_block(struct fun *f, int bi)
{
    struct blk *b = &f->b[bi];
    ndag = 0;
    memset(node_of, -1, sizeof node_of);
    int chain = -1;
    struct export_copy pending[MAX_VALS];
    int npending = 0;
    if ((bi == 0) & f->has_param) {
        /* LowerFormalArguments: the argument arrives in rdi. */
        chain = copy_to(-1, vr(vreg_of[0]), mk(N_REG, -1, -1, -1, ph(RDI)));
    }
    for (int i = b->nphi; i < b->n - 1; i++) {
        const struct inst *in = &b->code[i];
        int n;
        if (in->op == I_CALL) {
            chain = copy_to(chain, ph(RDI), operand(in->b));
            chain = mk(N_CALL, -1, -1, chain, in->a);
            dag[chain].has_res = in->d.kind == V_REG;
            n = chain;
            if (in->d.kind == V_REG) {
                n = mk(N_REG, -1, -1, chain, ph(RAX)); /* the result comes back in rax */
                chain = n;
            }
        } else {
            const int x = operand(in->a);
            const int y = operand(in->b);
            n = mk(N_ADD + (in->op - I_ADD), x, y, -1, NONE);
        }
        if (in->d.kind == V_REG) {
            node_of[in->d.n] = n;
            if (vreg_of[in->d.n] >= 0) {
                pending[npending++] = (struct export_copy){vreg_of[in->d.n], n};
            }
        }
    }

    /* The PHIs of the successors take their value from a virtual register that this block
     * fills; a constant gets a new one. */
    const struct inst *t = &b->code[b->n - 1];
    for (int s = 0; s < b->nsucc; s++) {
        struct blk *to = &f->b[b->succ[s]];
        for (int j = 0; j < to->nphi; j++) {
            const struct inst *phi = &to->code[j];
            const struct val v = phi->in[phi->inb[0] == bi ? 0 : 1];
            int src = v.kind == V_IMM ? new_vreg() : vreg_of[v.n];
            if (v.kind == V_IMM) {
                pending[npending++] = (struct export_copy){src, operand(v)};
            }
            struct inst *mphi = &to->m[j];
            mphi->in[mphi->nin] = vr(src);
            mphi->inb[mphi->nin++] = bi;
        }
    }
    for (int i = 0; i < npending; i++) {
        chain = copy_to(chain, vr(pending[i].vreg), pending[i].node);
    }

    if (t->op == I_BR) {
        chain = mk(N_BR, -1, -1, chain, t->a);
    } else if (t->op == I_CONDBR) {
        chain = mk(N_BRCOND, node_of[t->a.n], -1, chain, t->b);
        chain = mk(N_BR, -1, -1, chain, t->c);
    } else {
        struct val rv = NONE;
        if (t->a.kind != V_NONE) {
            rv = ph(RAX);
            chain = copy_to(chain, rv, operand(t->a));
        }
        chain = mk(N_RET, -1, -1, chain, rv);
    }

    P("bb.%d.%s:\n", bi, b->name);
    print_dag("DAG as built", 0);
    select_nodes();
    print_dag("DAG after selection", 1);
    MB = b;
    emit(chain);
}

static void stage_isel(void)
{
    P("== stage_isel: SelectionDAG per block ==\n");
    for (int fi = 0; fi < nfun; fi++) {
        struct fun *f = &funs[fi];
        MF = f;
        P("%s:\n", f->name);
        assign_vregs(f);
        for (int bi = 0; bi < f->nb; bi++) {
            struct blk *b = &f->b[bi];
            b->mn = 0;
            b->nphi = 0;
            while (b->code[b->nphi].op == I_PHI) {
                b->m[b->mn] = (struct inst){.op = M_PHI, .d = vr(vreg_of[b->code[b->nphi].d.n])};
                b->mn++;
                b->nphi++;
            }
        }
        for (int bi = 0; bi < f->nb; bi++) {
            /* The PHIs are the first machine instructions of a block. isel_block fills in their
             * operands while it visits the predecessors, which may come later in the layout. */
            isel_block(f, bi);
        }
    }
    print_mir("stage_isel: machine IR after scheduling, virtual registers and PHIs", 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_phi_elim
 *
 * PHIElimination. A PHI "d = PHI s1, bb1, s2, bb2" becomes a copy "d = COPY t" at the top of
 * its block and a copy "t = COPY s_i" at the end of each predecessor, just before its
 * terminators. The fresh register t is what keeps the copies of several PHIs from clobbering
 * each other.
 * ---------------------------------------------------------------------------------------- */

static void stage_phi_elim(void)
{
    for (int fi = 0; fi < nfun; fi++) {
        struct fun *f = &funs[fi];
        MF = f;
        for (int bi = 0; bi < f->nb; bi++) {
            struct blk *b = &f->b[bi];
            const int np = b->nphi;
            struct inst phis[2];
            memcpy(phis, b->m, (size_t)np * sizeof phis[0]);
            memmove(b->m, &b->m[np], (size_t)(b->mn - np) * sizeof b->m[0]);
            b->mn -= np;
            for (int k = 0; k < np; k++) {
                const struct val incoming = vr(new_vreg());
                minsert(b, k, (struct inst){.op = M_COPY, .d = phis[k].d, .a = incoming});
                for (int j = 0; j < phis[k].nin; j++) {
                    struct blk *pred = &f->b[phis[k].inb[j]];
                    minsert(pred, first_terminator(pred),
                            (struct inst){.op = M_COPY, .d = incoming, .a = phis[k].in[j]});
                }
            }
            b->nphi = 0;
        }
    }
    print_mir("stage_phi_elim: PHIs replaced by copies", 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_two_address
 *
 * An x86 arithmetic instruction overwrites its first source. "A = op B, C" becomes
 * "A = COPY B" followed by "A = op A, C". A now has two definitions, so the code is no longer
 * in SSA form.
 * ---------------------------------------------------------------------------------------- */

static void stage_two_address(void)
{
    for (int fi = 0; fi < nfun; fi++) {
        struct fun *f = &funs[fi];
        for (int bi = 0; bi < f->nb; bi++) {
            struct blk *b = &f->b[bi];
            for (int i = 0; i < b->mn; i++) {
                const int op = b->m[i].op;
                if ((op == M_ADD64rr) | (op == M_ADD64ri8) | (op == M_SUB64ri8) |
                    (op == M_IMUL64rr)) {
                    minsert(b, i, (struct inst){.op = M_COPY, .d = b->m[i].d, .a = b->m[i].a});
                    b->m[i + 1].a = b->m[i + 1].d;
                    i++;
                }
            }
        }
    }
    print_mir("stage_two_address: A = op B, C rewritten as A = B; A = op A, C", 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_live_intervals
 *
 * Every instruction gets two slots: 2*i for its uses and 2*i+1 for its defs. A virtual
 * register's interval is a list of segments [start, end) in slot order; a register that is
 * live across a block boundary has a segment that covers the whole stretch in between.
 * Physical registers get fixed ranges, from the instruction that sets them to the one that
 * reads them, and a call is a register mask at its slot that clobbers every caller-saved
 * register.
 * ---------------------------------------------------------------------------------------- */

static void number_slots(struct fun *f)
{
    int g = 0;
    for (int bi = 0; bi < f->nb; bi++) {
        f->b[bi].first_slot = 2 * g;
        for (int i = 0; i < f->b[bi].mn; i++) {
            f->b[bi].m[i].slot = 2 * g++;
        }
    }
}

static void add_segment(struct interval *iv, int s, int e)
{
    if ((iv->nseg > 0) && iv->e[iv->nseg - 1] >= s) {
        iv->e[iv->nseg - 1] = e > iv->e[iv->nseg - 1] ? e : iv->e[iv->nseg - 1];
        return;
    }
    iv->s[iv->nseg] = s;
    iv->e[iv->nseg++] = e;
}

static int overlaps(const struct interval *x, const struct interval *y)
{
    for (int i = 0; i < x->nseg; i++) {
        for (int j = 0; j < y->nseg; j++) {
            if ((x->s[i] < y->e[j]) & (y->s[j] < x->e[i])) {
                return 1;
            }
        }
    }
    return 0;
}

static void uses_of(const struct inst *in, struct val *out_uses)
{
    out_uses[0] = in->a;
    out_uses[1] = in->b;
}

static void build_intervals(struct fun *f)
{
    number_slots(f);
    memset(f->ivl, 0, sizeof f->ivl);
    memset(f->fixed, 0, sizeof f->fixed);
    f->ncall = 0;
    unsigned char gen[MAX_BLOCKS][MAX_VALS] = {{0}}, kill[MAX_BLOCKS][MAX_VALS] = {{0}};
    for (int bi = 0; bi < f->nb; bi++) {
        struct blk *b = &f->b[bi];
        memset(b->live_in, 0, sizeof b->live_in);
        memset(b->live_out, 0, sizeof b->live_out);
        for (int i = 0; i < b->mn; i++) {
            struct val u[2];
            uses_of(&b->m[i], u);
            for (int k = 0; k < 2; k++) {
                if (u[k].kind == V_REG) {
                    gen[bi][u[k].n] |= !kill[bi][u[k].n];
                }
            }
            if (b->m[i].d.kind == V_REG) {
                kill[bi][b->m[i].d.n] = 1;
            }
        }
    }
    int changed;
    do {
        changed = 0;
        for (int bi = f->nb - 1; bi >= 0; bi--) {
            struct blk *b = &f->b[bi];
            for (int v = 0; v < f->nvregs; v++) {
                int out_v = 0;
                for (int s = 0; s < b->nsucc; s++) {
                    out_v |= f->b[b->succ[s]].live_in[v];
                }
                const int in_v = gen[bi][v] | (out_v & !kill[bi][v]);
                changed |= (in_v != b->live_in[v]) | (out_v != b->live_out[v]);
                b->live_in[v] = (unsigned char)in_v;
                b->live_out[v] = (unsigned char)out_v;
            }
        }
    } while (changed);

    for (int v = 0; v < f->nvregs; v++) {
        struct interval *iv = &f->ivl[v];
        iv->phys = -1;
        for (int bi = 0; bi < f->nb; bi++) {
            const struct blk *b = &f->b[bi];
            const int block_end = b->first_slot + 2 * b->mn;
            int start = b->live_in[v] ? b->first_slot : -1;
            int end = b->first_slot;
            for (int i = 0; i < b->mn; i++) {
                struct val u[2];
                uses_of(&b->m[i], u);
                for (int k = 0; k < 2; k++) {
                    if ((u[k].kind == V_REG) & (u[k].n == v)) {
                        end = b->m[i].slot + 1;
                        iv->refs++;
                    }
                }
                if ((b->m[i].d.kind == V_REG) & (b->m[i].d.n == v)) {
                    start = start < 0 ? b->m[i].slot + 1 : start;
                    end = end > b->m[i].slot + 2 ? end : b->m[i].slot + 2;
                    iv->refs++;
                }
            }
            if (b->live_out[v]) {
                end = block_end;
            }
            if (start >= 0) {
                add_segment(iv, start, end);
            }
        }
        const int size = iv->nseg ? iv->e[iv->nseg - 1] - iv->s[0] : 1;
        iv->weight = iv->refs * 1000 / size;
    }

    for (int bi = 0; bi < f->nb; bi++) {
        const struct blk *b = &f->b[bi];
        for (int i = 0; i < b->mn; i++) {
            const struct inst *in = &b->m[i];
            if (in->op == M_CALL) {
                f->call_slot[f->ncall++] = in->slot + 1;
            }
            const struct val u[2] = {in->a, in->b};
            for (int k = 0; k < 2; k++) {
                if (u[k].kind != V_PHYS) {
                    continue;
                }
                /* The instruction that sets the register is the one before (isel glues them), or
                 * the register is an argument and was set before the function began. */
                const int start = i > 0 ? b->m[i - 1].slot + 1 : b->first_slot;
                struct interval *fx = &f->fixed[u[k].n];
                fx->s[fx->nseg] = start;
                fx->e[fx->nseg++] = in->slot + 1;
            }
        }
    }
}

static void print_intervals(const struct fun *f)
{
    P("%s:\n", f->name);
    for (int v = 0; v < f->nvregs; v++) {
        const struct interval *iv = &f->ivl[v];
        P("  %%%d", v);
        for (int i = 0; i < iv->nseg; i++) {
            P(" [%d,%d)", iv->s[i], iv->e[i]);
        }
        P("  weight %d\n", iv->weight);
    }
    for (int r = 0; r < 16; r++) {
        const struct interval *fx = &f->fixed[r];
        if (fx->nseg == 0) {
            continue;
        }
        P("  $%s fixed", reg_names[r]);
        for (int i = 0; i < fx->nseg; i++) {
            P(" [%d,%d)", fx->s[i], fx->e[i]);
        }
        P("\n");
    }
    for (int i = 0; i < f->ncall; i++) {
        P("  call at slot %d clobbers every caller-saved register\n", f->call_slot[i]);
    }
}

static void stage_live_intervals(void)
{
    for (int fi = 0; fi < nfun; fi++) {
        build_intervals(&funs[fi]);
    }
    print_mir("stage_live_intervals: machine IR with slot indexes", 1);
    P("== stage_live_intervals: intervals ==\n");
    for (int fi = 0; fi < nfun; fi++) {
        print_intervals(&funs[fi]);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_coalesce
 *
 * RegisterCoalescer. For "A = COPY B" between virtual registers, if the intervals of A and B
 * do not overlap, the two registers are joined into one and the copy disappears.
 * ---------------------------------------------------------------------------------------- */

static int join_one(struct fun *f)
{
    for (int bi = 0; bi < f->nb; bi++) {
        struct blk *b = &f->b[bi];
        for (int i = 0; i < b->mn; i++) {
            const struct inst *in = &b->m[i];
            if (!((in->op == M_COPY) & (in->d.kind == V_REG) & (in->a.kind == V_REG))) {
                continue;
            }
            const int d = (int)in->d.n, s = (int)in->a.n;
            if (overlaps(&f->ivl[d], &f->ivl[s])) {
                continue;
            }
            P("%s: joined %%%d into %%%d\n", f->name, s, d);
            for (int bj = 0; bj < f->nb; bj++) {
                for (int j = 0; j < f->b[bj].mn; j++) {
                    struct inst *x = &f->b[bj].m[j];
                    struct val *ops[] = {&x->d, &x->a, &x->b};
                    for (int k = 0; k < 3; k++) {
                        if ((ops[k]->kind == V_REG) & (ops[k]->n == s)) {
                            ops[k]->n = d;
                        }
                    }
                }
            }
            memmove(&b->m[i], &b->m[i + 1], (size_t)(b->mn - i - 1) * sizeof b->m[0]);
            b->mn--;
            return 1;
        }
    }
    return 0;
}

static void stage_coalesce(void)
{
    P("== stage_coalesce: copies joined ==\n");
    for (int fi = 0; fi < nfun; fi++) {
        while (join_one(&funs[fi])) {
            build_intervals(&funs[fi]);
        }
    }
    print_mir("stage_coalesce: machine IR after coalescing", 1);
}

/* ------------------------------------------------------------------------------------------
 * stage_regalloc
 *
 * RABasic. The intervals wait in a priority queue ordered by spill weight (ties go to the
 * higher register number). The next interval tries the registers in the allocation order,
 * with the registers that its copies point at (its hints) first, and takes the first one with
 * no interference: not with a fixed range, not with an interval already assigned to that
 * register, and, for a caller-saved register, not with a call inside the interval.
 * ---------------------------------------------------------------------------------------- */

static int interferes(const struct fun *f, int v, int p)
{
    const struct interval *iv = &f->ivl[v];
    if (overlaps(iv, &f->fixed[p])) {
        return 1;
    }
    if (p != RBX) {
        for (int c = 0; c < f->ncall; c++) {
            for (int i = 0; i < iv->nseg; i++) {
                if ((iv->s[i] <= f->call_slot[c]) & (f->call_slot[c] < iv->e[i])) {
                    return 1;
                }
            }
        }
    }
    for (int w = 0; w < f->nvregs; w++) {
        if ((f->ivl[w].phys == p) & overlaps(iv, &f->ivl[w])) {
            return 1;
        }
    }
    return 0;
}

static void allocate_function(struct fun *f)
{
    unsigned char queued[MAX_VALS] = {0};
    for (int v = 0; v < f->nvregs; v++) {
        queued[v] = f->ivl[v].nseg > 0;
    }
    for (;;) {
        int best = -1, best_key = -1;
        for (int v = 0; v < f->nvregs; v++) {
            const int key = f->ivl[v].weight * 64 + v; /* the weight, then the register number */
            if (queued[v] & (key > best_key)) {
                best = v;
                best_key = key;
            }
        }
        if (best < 0) {
            break;
        }
        queued[best] = 0;

        int order[NUM_ORDER + 4], n = 0;
        for (int bi = 0; bi < f->nb; bi++) {
            for (int i = 0; i < f->b[bi].mn; i++) {
                const struct inst *in = &f->b[bi].m[i];
                if (in->op != M_COPY) {
                    continue;
                }
                const struct val other = same(in->d, vr(best)) ? in->a : in->d;
                const int is_mine = same(in->d, vr(best)) | same(in->a, vr(best));
                const int hint = other.kind == V_PHYS ? (int)other.n : f->ivl[other.n].phys;
                if (is_mine & (hint >= 0) & (n < 4)) {
                    order[n++] = hint;
                }
            }
        }
        const int hints = n;
        for (int k = 0; k < NUM_ORDER; k++) {
            order[n++] = alloc_order[k];
        }
        int chosen = -1;
        for (int k = 0; (k < n) && chosen < 0; k++) {
            if (!interferes(f, best, order[k])) {
                chosen = order[k];
            }
        }
        require(chosen >= 0, "a spill, since every register interferes");
        f->ivl[best].phys = chosen;
        P("%s: %%%d weight %d, %d hints -> $%s\n", f->name, best, f->ivl[best].weight, hints,
          reg_names[chosen]);
        f->uses_rbx |= chosen == RBX;
    }

    /* VirtRegRewriter: replace the registers and drop the copies that became no-ops. */
    for (int bi = 0; bi < f->nb; bi++) {
        struct blk *b = &f->b[bi];
        int w = 0;
        for (int i = 0; i < b->mn; i++) {
            struct inst x = b->m[i];
            struct val *ops[] = {&x.d, &x.a, &x.b};
            for (int k = 0; k < 3; k++) {
                if (ops[k]->kind == V_REG) {
                    *ops[k] = ph(f->ivl[ops[k]->n].phys);
                }
            }
            if (!((x.op == M_COPY) & same(x.d, x.a))) {
                b->m[w++] = x;
            }
        }
        b->mn = w;
    }
}

static void stage_regalloc(void)
{
    P("== stage_regalloc: assignments, in the order they were made ==\n");
    for (int fi = 0; fi < nfun; fi++) {
        allocate_function(&funs[fi]);
    }
    print_mir("stage_regalloc: machine IR with physical registers", 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_prologue_epilogue
 *
 * A function that used a callee-saved register pushes it first and pops it before each
 * return. There are no stack slots, so there is no frame pointer or stack adjustment.
 * ---------------------------------------------------------------------------------------- */

static void stage_prologue_epilogue(void)
{
    for (int fi = 0; fi < nfun; fi++) {
        struct fun *f = &funs[fi];
        if (!f->uses_rbx) {
            continue;
        }
        minsert(&f->b[0], 0, (struct inst){.op = M_PUSH, .a = ph(RBX)});
        for (int bi = 0; bi < f->nb; bi++) {
            struct blk *b = &f->b[bi];
            if (b->m[b->mn - 1].op == M_RET) {
                minsert(b, b->mn - 1, (struct inst){.op = M_POP, .d = ph(RBX)});
            }
        }
    }
    print_mir("stage_prologue_epilogue: callee-saved registers saved and restored", 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_encode
 *
 * Every machine instruction becomes x86-64 bytes. A call, a jump and a string address are
 * rel32 fields whose target is only known later, so they are recorded and patched at the end.
 * The runtime (print_int and print_str) and the string follow the functions in the same
 * buffer. A jump to the block that follows is not emitted.
 * ---------------------------------------------------------------------------------------- */

#define CODE_SIZE 4096
static uint8_t out[CODE_SIZE];
static size_t out_len;

enum { FIX_BLOCK, FIX_FUN, FIX_PRINT_INT, FIX_PRINT_STR, FIX_STRING };
struct fixup {
    size_t at;
    int kind, fun, target;
};
static struct fixup fixups[32];
static int fixup_count;
static size_t block_off[MAX_FUNS][MAX_BLOCKS];
static size_t fun_off[MAX_FUNS];
static size_t print_int_off, print_str_off, string_off;

#define EMIT(...)                                                                                  \
    do {                                                                                           \
        const uint8_t bytes_[] = {__VA_ARGS__};                                                    \
        memcpy(&out[out_len], bytes_, sizeof bytes_);                                              \
        out_len += sizeof bytes_;                                                                  \
    } while (0)

static void emit32(uint32_t v)
{
    EMIT((uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24));
}

static void emit_rel32(int kind, int fun, int target)
{
    fixups[fixup_count++] = (struct fixup){out_len, kind, fun, target};
    emit32(0);
}

static void emit_rr(uint8_t op, int reg, int rm)
{
    EMIT(0x48 | ((reg >> 3) << 2) | (rm >> 3), op, 0xc0 | ((reg & 7) << 3) | (rm & 7));
}

static void emit_ri(uint8_t op, int ext, int rm, int64_t value)
{
    EMIT(0x48 | (rm >> 3), op, 0xc0 | (ext << 3) | (rm & 7));
    emit32((uint32_t)value);
}

static void emit_ri8(int ext, int rm, int64_t value)
{
    EMIT(0x48 | (rm >> 3), 0x83, 0xc0 | (ext << 3) | (rm & 7), (uint8_t)value);
}

static void emit_inst(int fi, int bi, int next, const struct inst *in)
{
    const int dst = (int)in->d.n;
    switch (in->op) {
    case M_PUSH:
        EMIT(0x50 + (uint8_t)in->a.n);
        break;
    case M_POP:
        EMIT(0x58 + (uint8_t)in->d.n);
        break;
    case M_MOV64ri32:
        emit_ri(0xc7, 0, dst, in->a.n);
        break;
    case M_LEA64r:
        EMIT(0x48 | ((dst >> 3) << 2), 0x8d, ((dst & 7) << 3) | 5);
        emit_rel32(FIX_STRING, fi, 0);
        break;
    case M_COPY:
        emit_rr(0x89, (int)in->a.n, dst);
        break;
    case M_ADD64rr:
        emit_rr(0x01, (int)in->b.n, dst);
        break;
    case M_IMUL64rr:
        EMIT(0x48 | ((dst >> 3) << 2) | ((int)in->b.n >> 3), 0x0f, 0xaf,
             0xc0 | ((dst & 7) << 3) | ((int)in->b.n & 7));
        break;
    case M_ADD64ri8:
        emit_ri8(0, dst, in->b.n);
        break;
    case M_SUB64ri8:
        emit_ri8(5, dst, in->b.n);
        break;
    case M_CMP64ri8:
        emit_ri8(7, (int)in->a.n, in->b.n);
        break;
    case M_JCC:
        EMIT(0x0f, 0x8c); /* jl */
        emit_rel32(FIX_BLOCK, fi, (int)in->a.n);
        break;
    case M_JMP:
        if (in->a.n != next) {
            EMIT(0xe9);
            emit_rel32(FIX_BLOCK, fi, (int)in->a.n);
        }
        break;
    case M_CALL:
        EMIT(0xe8);
        emit_rel32(in->a.n == NATIVE_PRINT_INT   ? FIX_PRINT_INT
                   : in->a.n == NATIVE_PRINT_STR ? FIX_PRINT_STR
                                                 : FIX_FUN,
                   fi, (int)in->a.n);
        break;
    default: /* M_RET */
        EMIT(0xc3);
        break;
    }
    (void)bi;
}

static void stage_encode(void)
{
    P("== stage_encode: machine code, with the offset of each instruction ==\n");
    for (int fi = 0; fi < nfun; fi++) {
        const struct fun *f = &funs[fi];
        fun_off[fi] = out_len;
        P("%s:\n", f->name);
        for (int bi = 0; bi < f->nb; bi++) {
            block_off[fi][bi] = out_len;
            for (int i = 0; i < f->b[bi].mn; i++) {
                P("  +0x%04zx  ", out_len);
                print_minst(&f->b[bi].m[i]);
                emit_inst(fi, bi, bi + 1, &f->b[bi].m[i]);
            }
        }
    }

    /* print_int(rdi): mov rax, rdi; then the digits are built backwards on the stack. */
    print_int_off = out_len;
    EMIT(0x48, 0x89, 0xf8, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x74, 0x24, 0x20,
         0x48, 0xc7, 0xc1, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xff, 0xce, 0xc6, 0x06, 0x0a,
         0x31, 0xd2, 0x48, 0xf7, 0xf1, 0x80, 0xc2, 0x30, 0x48, 0xff, 0xce, 0x88, 0x16,
         0x48, 0x85, 0xc0, 0x75, 0xee,
         0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0x29, 0xf2, 0xb8, 0x01, 0x00, 0x00, 0x00,
         0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x20, 0xc3);
    /* print_str(rdi): rdi points at a length byte followed by the characters. */
    print_str_off = out_len;
    EMIT(0x0f, 0xb6, 0x17, 0x48, 0x8d, 0x77, 0x01, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x01,
         0x00, 0x00, 0x00, 0x0f, 0x05, 0xc3);
    string_off = out_len;
    out[out_len++] = (uint8_t)strlen(HELLO_STRING);
    memcpy(&out[out_len], HELLO_STRING, strlen(HELLO_STRING));
    out_len += strlen(HELLO_STRING);

    for (int i = 0; i < fixup_count; i++) {
        const struct fixup fx = fixups[i];
        const size_t target = fx.kind == FIX_BLOCK       ? block_off[fx.fun][fx.target]
                              : fx.kind == FIX_FUN       ? fun_off[fx.target]
                              : fx.kind == FIX_PRINT_INT ? print_int_off
                              : fx.kind == FIX_PRINT_STR ? print_str_off
                                                         : string_off;
        const uint32_t rel = (uint32_t)(int32_t)((int64_t)target - (int64_t)(fx.at + 4));
        for (int j = 0; j < 4; j++) {
            out[fx.at + (size_t)j] = (uint8_t)(rel >> (8 * j));
        }
        P("+0x%04zx  rel32 %d to +0x%04zx\n", fx.at, (int32_t)rel, target);
    }
    P("== stage_encode: final code, %zu bytes ==", out_len);
    for (size_t i = 0; i < out_len; i++) {
        if (i % 16 == 0) {
            P("\n+0x%04zx:", i);
        }
        P(" %02x", out[i]);
    }
    P("\n");
}

/* ------------------------------------------------------------------------------------------
 * stage_export
 * ---------------------------------------------------------------------------------------- */

static void stage_export(const char *path)
{
    P("== stage_export ==\n");
    if (path != NULL) {
        FILE *fp = fopen(path, "wb");
        // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
        if (fp == NULL || fwrite(out, 1, out_len, fp) != out_len || fclose(fp) != 0) {
            perror(path);
            exit(1);
        }
        // GCOVR_EXCL_STOP
    }
    void *buf = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (buf == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    memcpy(buf, out, out_len);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (mprotect(buf, CODE_SIZE, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    /* ISO C forbids casting an object pointer to a function pointer, so copy the bits. */
    void *entry = (uint8_t *)buf + fun_off[nfun - 1];
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
    stage_irgen(program);
    stage_mem2reg();
    stage_isel();
    stage_phi_elim();
    stage_two_address();
    stage_live_intervals();
    stage_coalesce();
    stage_regalloc();
    stage_prologue_epilogue();
    stage_encode();
    stage_export(argc > 2 ? argv[2] : NULL);
    return 0;
}
