/*
 * gcc: a cut-down version of GCC, the GNU Compiler Collection, for x86-64.
 *
 * GCC lowers a function through three IRs. The front end builds GENERIC, a tree. The
 * gimplifier flattens it into GIMPLE, three-address statements with temporaries, and lowering
 * turns every return into a jump to one return. GIMPLE is split into basic blocks (the CFG)
 * and put into SSA form with PHI nodes. Then expand turns the SSA form into RTL, a list of
 * register transfers on pseudo registers, with the SSA names of one variable sharing a pseudo.
 * RTL passes follow: combine merges instructions, IRA gives every pseudo a hard register by
 * graph coloring, the prologue and epilogue are added, and final prints assembly from the
 * output template of each instruction.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout, the intermediate form after each stage on stderr,
 *   and writes the machine code to code.bin if a path is given.
 *
 * Stages, in the order of the real pipeline:
 *   stage_ast        c-parser and c-fold: GENERIC, hand built in the shape the front end gives
 *   stage_gimplify   gimplify.c: GIMPLE with temporaries _N, labels and gotos
 *   stage_lower      gimple-low.c: every return becomes a goto to one return
 *   stage_cfg        tree-cfg.c: basic blocks and edges, labels and gotos removed
 *   stage_ssa        tree-into-ssa.c: dominators, dominance frontiers, PHIs, renaming
 *   stage_expand     tree-outof-ssa.c and cfgexpand.c: partitions, then RTL
 *   stage_combine    combine.c: make_more_copies, then copies merged into their user
 *   stage_ira        ira-color.c: graph coloring of the pseudos, with copy preferences
 *   stage_frame      function.c: the registers to save and the stack alignment
 *   stage_final      final.c: assembly text from the output templates
 *   stage_encode     stands in for the assembler (GCC runs `as`): bytes and rel32 fields
 *   stage_export     stands in for the linker and the loader: map the bytes and run main
 */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

#define MAX_FUNCS 2
#define MAX_TREES 16
#define MAX_GSTMTS 16
#define MAX_STMTS 24
#define MAX_BLOCKS 8
#define MAX_BS 12 /* statements and PHIs in one block */
#define MAX_VARS 4
#define MAX_VERS 32
#define MAX_INSNS 64
#define MAX_REGS 128
#define MAX_LABELS 16
#define MAX_CODE 160
#define NAME 12
#define SYM 24
#define FIRST_PSEUDO 98
#define HELLO_STRING "Hello, world!\n"

/* Only the shapes that the examples need are handled. A shape that none of them contains stops
 * the program here. */
static void require(int ok, const char *what)
{
    // GCOVR_EXCL_START: every shape in the examples is handled
    if (!ok) {
        fprintf(stderr, "unsupported: %s\n", what);
        exit(1);
    }
    // GCOVR_EXCL_STOP
}

/* ------------------------------------------------------------------------------------------
 * Data. One function record holds the form of the function in every stage.
 * ---------------------------------------------------------------------------------------- */

/* GENERIC: expression trees and statements that may nest. */
enum { T_CONST, T_VAR, T_STR, T_ADD, T_MUL, T_CALL };
struct tree {
    int k, val, a, b;
    const char *name;
};
enum { G_ASSIGN, G_EXPR, G_RETURN, G_IF, G_LABEL, G_GOTO, G_COND };
enum { C_LT, C_LE, C_GE, C_GT };
static const char *const cmp_names[] = {"<", "<=", ">=", ">"};
static const int cmp_inverse[] = {C_GE, C_GT, C_LT, C_LE};
struct gstmt {
    int k, var, expr, cmp, ca, cb, l1, l2;
    int then[2], nthen, els[2], nels;
};

/* GIMPLE operands. After SSA an operand is a name: a version of a variable, or a temporary. */
enum { O_NONE, O_CONST, O_VAR, O_SSA, O_STR };
struct opd {
    int k, v, ver, dflt;
    const char *name; /* the variable, or NULL for an anonymous name printed as _N */
};
enum { B_NONE, B_ADD, B_MUL };
enum { S_ASSIGN, S_CALL, S_COND, S_GOTO, S_LABEL, S_RETURN, S_PHI };
struct stmt {
    int k, bop, cmp, l1, l2, nargs;
    struct opd dst, a, b, args[3];
    const char *fn;
};

struct var {
    char name[NAME];
    int artificial; /* a D.nnnn temporary, which SSA renames to an anonymous name */
};

struct block {
    struct stmt s[MAX_BS];
    int ns, nphi, succ[2], nsucc, pred[3], npred, idom;
    uint32_t dom, df, livein;
};

/* RTL */
enum { X_NONE, X_REG, X_IMM };
struct rx {
    int k, v;
};
enum { R_MOVE, R_ADD, R_MUL, R_CMP, R_JCC, R_JMP, R_CALL, R_USE, R_LEA_SYM, R_LABEL };
struct insn {
    int code, uid, bb, dst, cond, target, has_arg, dead;
    struct rx a, b;
    const char *sym;
};
static const int insn_cost[] = {4, 4, 12, 4, 4, 4, 4, 0, 4, 0};

/* x86-64 registers in GCC's numbering, which differs from the hardware's. */
enum { AX, DX, CX, BX, SI, DI, NHARD };
static const char *const reg64[] = {"rax", "rdx", "rcx", "rbx", "rsi", "rdi"};
static const char *const reg32[] = {"eax", "edx", "ecx", "ebx", "esi", "edi"};
static const char *const reg_short[] = {"ax", "dx", "cx", "bx", "si", "di"};
static const int hw_reg[] = {0, 2, 1, 3, 6, 7};
static const int alloc_order[] = {AX, DX, CX, SI, DI, BX};

struct func {
    const char *name;
    int nparams;
    struct var vars[MAX_VARS];
    int nvars;
    struct tree trees[MAX_TREES];
    int ntrees;
    struct gstmt g[MAX_GSTMTS];
    int ng, body[16], nbody;
    struct stmt flat[MAX_STMTS];
    int nflat, ret_var, ntemps, next_ver;
    struct opd deflt[MAX_VARS];
    struct block bb[MAX_BLOCKS];
    int nbb;
    struct insn ins[MAX_INSNS];
    int nins, next_uid, next_pseudo;
    int preg[MAX_VERS], assign[MAX_REGS], label_no[MAX_BLOCKS];
    int push_bx, sub_rsp, has_call;
};
static struct func funcs[MAX_FUNCS];
static int nfuncs;
static int dnum = 1000; /* the next number for a D.nnnn name, which is a global counter */
static int have_str;

/* ------------------------------------------------------------------------------------------
 * stage_ast
 *
 * The C front end parses the program and folds constants before it hands GENERIC to the middle
 * end, so the trees here are written in the shape it produces:
 *  - (2 + 3) * 4 - 6 is already 14.
 *  - n < 2 is canonicalized to n <= 1, and n - 1 to n + -1.
 *  - n * fact (n - 1) becomes fact (n + -1) * n, with the call first.
 *  - A while loop is rotated: goto <cond>; <body>: ...; <cond>: if (c) goto <body>; else goto
 *    <exit>; <exit>:. The label numbers are made in the order exit, cond, body.
 * ---------------------------------------------------------------------------------------- */

static struct func *begin_func(const char *name, int nparams)
{
    struct func *f = &funcs[nfuncs++];
    f->name = name;
    f->nparams = nparams;
    f->ret_var = -1;
    return f;
}

static int add_var(struct func *f, const char *name)
{
    snprintf(f->vars[f->nvars].name, NAME, "%s", name);
    return f->nvars++;
}

static int tr(struct func *f, int k, int val, int a, int b, const char *name)
{
    f->trees[f->ntrees] = (struct tree){k, val, a, b, name};
    return f->ntrees++;
}

static int t_const(struct func *f, int v)
{
    return tr(f, T_CONST, v, -1, -1, NULL);
}

static int t_var(struct func *f, int v)
{
    return tr(f, T_VAR, v, -1, -1, NULL);
}

static int t_call(struct func *f, const char *name, int arg)
{
    return tr(f, T_CALL, 0, arg, -1, name);
}

static int t_bin(struct func *f, int k, int a, int b)
{
    return tr(f, k, 0, a, b, NULL);
}

static int gs(struct func *f, struct gstmt g)
{
    f->g[f->ng] = g;
    return f->ng++;
}

static void body(struct func *f, int id)
{
    f->body[f->nbody++] = id;
}

static void g_assign(struct func *f, int var, int expr)
{
    body(f, gs(f, (struct gstmt){.k = G_ASSIGN, .var = var, .expr = expr}));
}

static void g_call(struct func *f, const char *fn, int arg)
{
    body(f, gs(f, (struct gstmt){.k = G_EXPR, .expr = t_call(f, fn, arg)}));
}

static void g_print_int(struct func *f, int arg)
{
    g_call(f, "print_int", arg);
}

static void build_hello(void)
{
    struct func *f = begin_func("main", 0);
    g_call(f, "print_str", tr(f, T_STR, 0, -1, -1, NULL));
}

static void build_arithmetic(void)
{
    struct func *f = begin_func("main", 0);
    g_print_int(f, t_const(f, 14));
}

static void build_branch(void)
{
    struct func *f = begin_func("main", 0);
    const int a = add_var(f, "a");
    const int b = add_var(f, "b");
    g_assign(f, a, t_const(f, 3));
    g_assign(f, b, t_const(f, 5));
    const int then = gs(f, (struct gstmt){.k = G_EXPR, .expr = t_call(f, "print_int", t_var(f, b))});
    const int els = gs(f, (struct gstmt){.k = G_EXPR, .expr = t_call(f, "print_int", t_var(f, a))});
    body(f, gs(f, (struct gstmt){.k = G_IF, .cmp = C_LT, .ca = t_var(f, a), .cb = t_var(f, b),
                                 .then = {then}, .nthen = 1, .els = {els}, .nels = 1}));
}

static void build_loop(void)
{
    struct func *f = begin_func("main", 0);
    const int sum = add_var(f, "sum");
    const int i = add_var(f, "i");
    const int l_exit = dnum++;
    const int l_cond = dnum++;
    const int l_body = dnum++;
    g_assign(f, sum, t_const(f, 0));
    g_assign(f, i, t_const(f, 1));
    body(f, gs(f, (struct gstmt){.k = G_GOTO, .l1 = l_cond}));
    body(f, gs(f, (struct gstmt){.k = G_LABEL, .l1 = l_body}));
    g_assign(f, sum, t_bin(f, T_ADD, t_var(f, sum), t_var(f, i)));
    g_assign(f, i, t_bin(f, T_ADD, t_var(f, i), t_const(f, 1)));
    body(f, gs(f, (struct gstmt){.k = G_LABEL, .l1 = l_cond}));
    body(f, gs(f, (struct gstmt){.k = G_COND, .cmp = C_LE, .ca = t_var(f, i),
                                 .cb = t_const(f, 10), .l1 = l_body, .l2 = l_exit}));
    body(f, gs(f, (struct gstmt){.k = G_LABEL, .l1 = l_exit}));
    g_print_int(f, t_var(f, sum));
}

static void build_call(void)
{
    struct func *f = begin_func("fact", 1);
    const int n = add_var(f, "n");
    const int one = gs(f, (struct gstmt){.k = G_RETURN, .expr = t_const(f, 1)});
    body(f, gs(f, (struct gstmt){.k = G_IF, .cmp = C_LE, .ca = t_var(f, n), .cb = t_const(f, 1),
                                 .then = {one}, .nthen = 1}));
    const int dec = t_bin(f, T_ADD, t_var(f, n), t_const(f, -1));
    body(f, gs(f, (struct gstmt){.k = G_RETURN,
                                 .expr = t_bin(f, T_MUL, t_call(f, "fact", dec), t_var(f, n))}));
    struct func *m = begin_func("main", 0);
    g_print_int(m, t_call(m, "fact", t_const(m, 5)));
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

static void fmt_tree(const struct func *f, int t, char *buf, size_t n)
{
    const struct tree *tn = &f->trees[t];
    char x[30], y[30];
    switch (tn->k) {
    case T_CONST:
        snprintf(buf, n, "%d", tn->val);
        break;
    case T_VAR:
        snprintf(buf, n, "%s", f->vars[tn->val].name);
        break;
    case T_STR:
        snprintf(buf, n, "\"Hello, world!\\n\"");
        break;
    case T_CALL:
        fmt_tree(f, tn->a, x, sizeof x);
        snprintf(buf, n, "%s (%s)", tn->name, x);
        break;
    default: /* T_ADD and T_MUL */
        fmt_tree(f, tn->a, x, sizeof x);
        fmt_tree(f, tn->b, y, sizeof y);
        snprintf(buf, n, "%s %s %s", x, tn->k == T_ADD ? "+" : "*", y);
        break;
    }
}

static void print_gstmt(const struct func *f, int id, int indent)
{
    const struct gstmt *g = &f->g[id];
    char x[64], y[64];
    P("%*s", indent, "");
    switch (g->k) {
    case G_ASSIGN:
        fmt_tree(f, g->expr, x, sizeof x);
        P("%s = %s;\n", f->vars[g->var].name, x);
        break;
    case G_EXPR:
        fmt_tree(f, g->expr, x, sizeof x);
        P("%s;\n", x);
        break;
    case G_RETURN:
        fmt_tree(f, g->expr, x, sizeof x);
        P("return %s;\n", x);
        break;
    case G_LABEL:
        P("<D.%d>:\n", g->l1);
        break;
    case G_GOTO:
        P("goto <D.%d>;\n", g->l1);
        break;
    case G_COND:
        fmt_tree(f, g->ca, x, sizeof x);
        fmt_tree(f, g->cb, y, sizeof y);
        P("if (%s %s %s) goto <D.%d>; else goto <D.%d>;\n", x, cmp_names[g->cmp], y, g->l1,
          g->l2);
        break;
    default: /* G_IF */
        fmt_tree(f, g->ca, x, sizeof x);
        fmt_tree(f, g->cb, y, sizeof y);
        P("if (%s %s %s)\n%*s{\n", x, cmp_names[g->cmp], y, indent + 2, "");
        for (int i = 0; i < g->nthen; i++) {
            print_gstmt(f, g->then[i], indent + 4);
        }
        P("%*s}\n", indent + 2, "");
        if (g->nels > 0) {
            P("%*selse\n%*s{\n", indent, "", indent + 2, "");
            for (int i = 0; i < g->nels; i++) {
                print_gstmt(f, g->els[i], indent + 4);
            }
            P("%*s}\n", indent + 2, "");
        }
        break;
    }
}

static void print_header(const struct func *f)
{
    if (f->nparams > 0) {
        P("%s (int %s)\n{\n", f->name, f->vars[0].name);
    } else {
        P("%s ()\n{\n", f->name);
    }
}

static void stage_ast(const struct program *program)
{
    program->build();
    P("== stage_ast: GENERIC ==\n");
    for (int i = 0; i < nfuncs; i++) {
        const struct func *f = &funcs[i];
        print_header(f);
        for (int j = f->nparams; j < f->nvars; j++) {
            P("  int %s;\n", f->vars[j].name);
        }
        for (int j = 0; j < f->nbody; j++) {
            print_gstmt(f, f->body[j], 2);
        }
        P("}\n");
    }
}

/* ------------------------------------------------------------------------------------------
 * Printing GIMPLE, shared by the stages from stage_gimplify to stage_ssa.
 * ---------------------------------------------------------------------------------------- */

static void fmt_opd(char *buf, size_t n, struct opd o)
{
    switch (o.k) {
    case O_CONST:
        snprintf(buf, n, "%d", o.v);
        break;
    case O_VAR:
        snprintf(buf, n, "%s", o.name);
        break;
    case O_STR:
        snprintf(buf, n, "\"Hello, world!\\n\"");
        break;
    default: /* O_SSA */
        if (o.name != NULL) {
            snprintf(buf, n, "%s_%d", o.name, o.ver);
        } else {
            snprintf(buf, n, "_%d", o.ver);
        }
        if (o.dflt) {
            snprintf(buf + strlen(buf), n - strlen(buf), "(D)");
        }
        break;
    }
}

/* In a block, the targets of a condition are block numbers, otherwise D labels. */
static void fmt_stmt(char *buf, size_t n, const struct stmt *s, int in_cfg)
{
    char d[32], a[32], b[32];
    fmt_opd(d, sizeof d, s->dst);
    fmt_opd(a, sizeof a, s->a);
    fmt_opd(b, sizeof b, s->b);
    switch (s->k) {
    case S_ASSIGN:
        if (s->bop == B_NONE) {
            snprintf(buf, n, "%s = %s;", d, a);
        } else {
            snprintf(buf, n, "%s = %s %s %s;", d, a, s->bop == B_ADD ? "+" : "*", b);
        }
        break;
    case S_CALL:
        if (s->dst.k == O_NONE) {
            snprintf(buf, n, "%s (%s);", s->fn, a);
        } else {
            snprintf(buf, n, "%s = %s (%s);", d, s->fn, a);
        }
        break;
    case S_COND:
        if (in_cfg) {
            snprintf(buf, n, "if (%s %s %s) goto <bb %d>; else goto <bb %d>;", a,
                     cmp_names[s->cmp], b, s->l1, s->l2);
        } else {
            snprintf(buf, n, "if (%s %s %s) goto <D.%d>; else goto <D.%d>;", a,
                     cmp_names[s->cmp], b, s->l1, s->l2);
        }
        break;
    case S_GOTO:
        snprintf(buf, n, "goto <D.%d>;", s->l1);
        break;
    case S_LABEL:
        snprintf(buf, n, "<D.%d>:", s->l1);
        break;
    case S_RETURN:
        if (s->a.k == O_NONE) {
            snprintf(buf, n, "return;");
        } else {
            snprintf(buf, n, "return %s;", a);
        }
        break;
    default: { /* S_PHI */
        snprintf(buf, n, "# %s = PHI <", d);
        return;
    }
    }
}

/* Declarations as the real dumps print them. mode 0: gimple and lower, mode 1: cfg, mode 2:
 * ssa, where the anonymous names replace the D temporaries. */
static void print_decls(const struct func *f, int mode)
{
    const int first = f->nparams;
    if (mode == 0) {
        for (int j = first; j < f->nvars; j++) {
            P("  int %s;\n", f->vars[j].name);
        }
    } else {
        for (int j = f->nvars - 1; j >= first; j--) {
            if (!(f->vars[j].artificial & (mode == 2))) {
                P("  int %s;\n", f->vars[j].name);
            }
        }
    }
    if (mode == 2) {
        for (int v = 1; v < f->next_ver; v++) {
            int anon = 0;
            for (int b = 2; b < f->nbb; b++) {
                for (int i = 0; i < f->bb[b].ns; i++) {
                    const struct stmt *s = &f->bb[b].s[i];
                    anon |= (s->dst.k == O_SSA) & (s->dst.ver == v) & (s->dst.name == NULL);
                }
            }
            if (anon) {
                P("  int _%d;\n", v);
            }
        }
    }
    P("\n");
}

static void print_flat(const struct func *f, int mode)
{
    print_header(f);
    print_decls(f, mode);
    for (int i = 0; i < f->nflat; i++) {
        char text[96];
        fmt_stmt(text, sizeof text, &f->flat[i], 0);
        P("%s%s\n", f->flat[i].k == S_LABEL ? " " : "  ", text);
    }
    P("}\n");
}

/* ------------------------------------------------------------------------------------------
 * stage_gimplify
 *
 * gimplify.c flattens the trees. An operand that is not a constant or a variable is first
 * computed into a temporary, a name _N that is already in SSA form (make_ssa_name), and the
 * subexpressions are evaluated left to right, and the operands of + and * are swapped so that a
 * temporary comes last, as the real dumps show (the rule is probably tree_swap_operands_p, which
 * was not read). `if` becomes a condition that jumps to label_true
 * or label_false, with label_cont only when the then arm falls through and an else arm exists
 * (gimplify_cond_expr). Every return writes one shared return temporary D.nnnn
 * (gimplify_return_expr), which is made when the first return is met.
 * ---------------------------------------------------------------------------------------- */

static void emit(struct func *f, struct stmt s)
{
    f->flat[f->nflat++] = s;
}

static struct opd gimplify_operand(struct func *f, int t);

static void gimplify_rhs(struct func *f, struct opd dst, int t)
{
    const struct tree *tn = &f->trees[t];
    struct stmt s = {0};
    s.dst = dst;
    switch (tn->k) {
    case T_CALL:
        s.k = S_CALL;
        s.fn = tn->name;
        s.a = gimplify_operand(f, tn->a);
        break;
    case T_ADD:
    case T_MUL:
        s.k = S_ASSIGN;
        s.bop = tn->k == T_ADD ? B_ADD : B_MUL;
        s.a = gimplify_operand(f, tn->a);
        s.b = gimplify_operand(f, tn->b);
        if ((s.a.k == O_SSA) & (s.b.k != O_SSA)) {
            /* The dumps show the temporary last: `n * _2`, though the source has the call first. */
            const struct opd swap = s.a;
            s.a = s.b;
            s.b = swap;
        }
        break;
    default: /* a leaf */
        s.k = S_ASSIGN;
        s.a = gimplify_operand(f, t);
        break;
    }
    emit(f, s);
}

static struct opd gimplify_operand(struct func *f, int t)
{
    const struct tree *tn = &f->trees[t];
    switch (tn->k) {
    case T_CONST:
        return (struct opd){O_CONST, tn->val, 0, 0, NULL};
    case T_VAR:
        return (struct opd){O_VAR, tn->val, 0, 0, f->vars[tn->val].name};
    case T_STR:
        have_str = 1;
        return (struct opd){O_STR, 0, 0, 0, NULL};
    default: {
        /* The temporary is numbered after its operands, so that _1 is the innermost one. */
        const int save = f->nflat;
        struct opd tmp = {O_SSA, -1, 0, 0, NULL};
        gimplify_rhs(f, tmp, t);
        tmp.ver = ++f->ntemps;
        f->next_ver = f->ntemps + 1;
        f->flat[save + (f->nflat - save) - 1].dst = tmp;
        return tmp;
    }
    }
}

static void gimplify_stmt(struct func *f, int id);

static void gimplify_list(struct func *f, const int *ids, int n)
{
    for (int i = 0; i < n; i++) {
        gimplify_stmt(f, ids[i]);
    }
}

static void gimplify_stmt(struct func *f, int id)
{
    const struct gstmt *g = &f->g[id];
    switch (g->k) {
    case G_ASSIGN:
        gimplify_rhs(f, (struct opd){O_VAR, g->var, 0, 0, f->vars[g->var].name}, g->expr);
        break;
    case G_EXPR:
        gimplify_rhs(f, (struct opd){0}, g->expr);
        break;
    case G_RETURN: {
        if (f->ret_var < 0) {
            char name[NAME];
            snprintf(name, sizeof name, "D.%d", dnum++);
            f->ret_var = add_var(f, name);
            f->vars[f->ret_var].artificial = 1;
        }
        const struct opd rv = {O_VAR, f->ret_var, 0, 0, f->vars[f->ret_var].name};
        gimplify_rhs(f, rv, g->expr);
        emit(f, (struct stmt){.k = S_RETURN, .a = rv});
        break;
    }
    case G_LABEL:
        emit(f, (struct stmt){.k = S_LABEL, .l1 = g->l1});
        break;
    case G_GOTO:
        emit(f, (struct stmt){.k = S_GOTO, .l1 = g->l1});
        break;
    case G_COND: {
        const struct opd a = gimplify_operand(f, g->ca);
        const struct opd b = gimplify_operand(f, g->cb);
        emit(f, (struct stmt){.k = S_COND, .cmp = g->cmp, .a = a, .b = b, .l1 = g->l1,
                              .l2 = g->l2});
        break;
    }
    default: { /* G_IF */
        const int last = f->g[g->then[g->nthen - 1]].k;
        const int falls = (last != G_RETURN) & (last != G_GOTO);
        const int l_true = dnum++;
        const int l_false = dnum++;
        const int has_cont = (g->nels > 0) & falls;
        const int l_cont = has_cont ? dnum++ : 0;
        const struct opd a = gimplify_operand(f, g->ca);
        const struct opd b = gimplify_operand(f, g->cb);
        emit(f, (struct stmt){.k = S_COND, .cmp = g->cmp, .a = a, .b = b, .l1 = l_true,
                              .l2 = l_false});
        emit(f, (struct stmt){.k = S_LABEL, .l1 = l_true});
        gimplify_list(f, g->then, g->nthen);
        if (has_cont) {
            emit(f, (struct stmt){.k = S_GOTO, .l1 = l_cont});
        }
        emit(f, (struct stmt){.k = S_LABEL, .l1 = l_false});
        gimplify_list(f, g->els, g->nels);
        if (has_cont) {
            emit(f, (struct stmt){.k = S_LABEL, .l1 = l_cont});
        }
        break;
    }
    }
}

static void stage_gimplify(void)
{
    P("== stage_gimplify: GIMPLE ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        f->next_ver = 1;
        gimplify_list(f, f->body, f->nbody);
        print_flat(f, 0);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_lower
 *
 * lower_gimple_return turns every return into a goto to one label, and appends that label and
 * one return at the end. A function without a returned value gets a plain return appended.
 * ---------------------------------------------------------------------------------------- */

static void stage_lower(void)
{
    P("== stage_lower: GIMPLE after lowering ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        if (f->ret_var >= 0) {
            const int l_ret = dnum++;
            struct stmt ret = {.k = S_RETURN, .a = {O_VAR, f->ret_var, 0, 0, NULL}};
            ret.a.name = f->vars[f->ret_var].name;
            for (int j = 0; j < f->nflat; j++) {
                if (f->flat[j].k == S_RETURN) {
                    f->flat[j] = (struct stmt){.k = S_GOTO, .l1 = l_ret};
                }
            }
            emit(f, (struct stmt){.k = S_LABEL, .l1 = l_ret});
            emit(f, ret);
        } else {
            emit(f, (struct stmt){.k = S_RETURN});
        }
        print_flat(f, 0);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_cfg
 *
 * make_blocks_1 starts a block at a label and after a jump or return, and make_edges adds the
 * edges: a condition gets its true edge first, then its false edge, and a block that ends in
 * something else falls through. A simple goto becomes a plain edge and the goto is removed, and
 * so are the labels. Blocks are numbered from 2 (0 and 1 are the entry and the exit). Blocks
 * that cleanup would merge do not occur in the examples.
 * ---------------------------------------------------------------------------------------- */

static void add_edge(struct func *f, int from, int to)
{
    struct block *a = &f->bb[from];
    struct block *b = &f->bb[to];
    a->succ[a->nsucc++] = to;
    b->pred[b->npred++] = from;
}

static int lab_lookup(const int *ids, const int *bbs, int n, int id)
{
    int i = 0;
    while ((i < n) & (ids[i] != id)) {
        i++;
    }
    return bbs[i];
}

static void print_cfg(const struct func *f, int mode)
{
    print_header(f);
    print_decls(f, mode);
    for (int b = 2; b < f->nbb; b++) {
        const struct block *blk = &f->bb[b];
        P("  <bb %d> :\n", b);
        for (int i = 0; i < blk->ns; i++) {
            char text[160];
            const struct stmt *s = &blk->s[i];
            if (s->k == S_PHI) {
                int len;
                fmt_stmt(text, sizeof text, s, 1);
                len = (int)strlen(text);
                for (int j = 0; j < s->nargs; j++) {
                    char arg[32];
                    fmt_opd(arg, sizeof arg, s->args[j]);
                    len += snprintf(text + len, sizeof text - (size_t)len, "%s%s(%d)",
                                    j > 0 ? ", " : "", arg, blk->pred[j]);
                }
                snprintf(text + len, sizeof text - (size_t)len, ">");
            } else {
                fmt_stmt(text, sizeof text, s, 1);
            }
            P("  %s\n", text);
        }
        if ((blk->nsucc == 1) & (blk->succ[0] != b + 1)) {
            P("  goto <bb %d>;\n", blk->succ[0]);
        }
        P("\n");
    }
    P("}\n");
}

static void stage_cfg(void)
{
    P("== stage_cfg: basic blocks ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        int lab_id[MAX_LABELS], lab_bb[MAX_LABELS], nl = 0;
        int b = 2, pending = 0;
        f->nbb = 3;
        for (int j = 0; j < f->nflat; j++) {
            const struct stmt *s = &f->flat[j];
            if (pending | ((s->k == S_LABEL) & (f->bb[b].ns > 0))) {
                b = f->nbb++;
                pending = 0;
            }
            if (s->k == S_LABEL) {
                lab_id[nl] = s->l1;
                lab_bb[nl++] = b;
            }
            f->bb[b].s[f->bb[b].ns++] = *s;
            pending = (s->k == S_COND) | (s->k == S_GOTO) | (s->k == S_RETURN);
        }
        for (b = 2; b < f->nbb; b++) {
            struct block *blk = &f->bb[b];
            struct stmt *last = &blk->s[blk->ns - 1];
            switch (last->k) {
            case S_COND:
                last->l1 = lab_lookup(lab_id, lab_bb, nl, last->l1);
                last->l2 = lab_lookup(lab_id, lab_bb, nl, last->l2);
                add_edge(f, b, last->l1);
                add_edge(f, b, last->l2);
                break;
            case S_GOTO:
                add_edge(f, b, lab_lookup(lab_id, lab_bb, nl, last->l1));
                blk->ns--;
                break;
            case S_RETURN:
                break;
            default:
                add_edge(f, b, b + 1);
                break;
            }
        }
        for (b = 2; b < f->nbb; b++) {
            struct block *blk = &f->bb[b];
            if (blk->s[0].k == S_LABEL) {
                memmove(&blk->s[0], &blk->s[1], (size_t)(blk->ns - 1) * sizeof blk->s[0]);
                blk->ns--;
            }
        }
        print_cfg(f, 1);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_ssa
 *
 * Dominators, then dominance frontiers as in cfganal.c compute_dominance_frontiers: for every
 * join block, walk up from each predecessor to the join's immediate dominator. PHIs go at the
 * iterated dominance frontier of the blocks that assign a variable (compute_idf), for the
 * variables in order, and only where the variable is live on entry (the pruning of
 * prune_unused_phi_nodes). Renaming walks the dominator tree, children in descending block
 * order as GCC does. A variable used before any assignment gets a default definition, printed
 * with (D), the first time it is used (get_reaching_def).
 * ---------------------------------------------------------------------------------------- */

static uint32_t vbit(struct opd o)
{
    return (uint32_t)(o.k == O_VAR) << (o.v & 15);
}

static void compute_dominators(struct func *f)
{
    const uint32_t all = ((1u << f->nbb) - 1) & ~3u;
    for (int b = 2; b < f->nbb; b++) {
        f->bb[b].dom = all;
    }
    f->bb[2].dom = 1u << 2;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int b = 3; b < f->nbb; b++) {
            uint32_t d = all;
            for (int p = 0; p < f->bb[b].npred; p++) {
                d &= f->bb[f->bb[b].pred[p]].dom;
            }
            d |= 1u << b;
            changed |= d != f->bb[b].dom;
            f->bb[b].dom = d;
        }
    }
    for (int b = 3; b < f->nbb; b++) {
        const uint32_t strict = f->bb[b].dom & ~(1u << b);
        for (int d = 2; d < f->nbb; d++) {
            if (((strict >> d) & 1u) & (f->bb[d].dom == strict)) {
                f->bb[b].idom = d;
            }
        }
    }
    for (int b = 2; b < f->nbb; b++) {
        struct block *blk = &f->bb[b];
        if (blk->npred >= 2) {
            for (int p = 0; p < blk->npred; p++) {
                int runner = blk->pred[p];
                while (runner != blk->idom) {
                    f->bb[runner].df |= 1u << b;
                    runner = f->bb[runner].idom;
                }
            }
        }
    }
}

static struct opd new_def(struct func *f, struct opd var)
{
    const int artificial = f->vars[var.v].artificial;
    return (struct opd){O_SSA, var.v, f->next_ver++, 0, artificial ? NULL : var.name};
}

static struct opd reaching_def(struct func *f, const struct opd *cur, int var)
{
    if (cur[var].k != O_NONE) {
        return cur[var];
    }
    if (f->deflt[var].k == O_NONE) {
        struct opd v = {O_VAR, var, 0, 0, f->vars[var].name};
        f->deflt[var] = new_def(f, v);
        f->deflt[var].dflt = 1;
    }
    return f->deflt[var];
}

static struct opd rewrite_use(struct func *f, const struct opd *cur, struct opd o)
{
    if (o.k != O_VAR) {
        return o;
    }
    return reaching_def(f, cur, o.v);
}

static void rename_block(struct func *f, int b, const struct opd *incoming)
{
    struct opd cur[MAX_VARS];
    struct block *blk = &f->bb[b];
    memcpy(cur, incoming, sizeof cur);
    for (int i = 0; i < blk->ns; i++) {
        struct stmt *s = &blk->s[i];
        if (s->k != S_PHI) {
            s->a = rewrite_use(f, cur, s->a);
            s->b = rewrite_use(f, cur, s->b);
        }
        if (s->k == S_PHI) {
            cur[s->dst.v] = s->dst;
        } else if (s->dst.k == O_VAR) {
            s->dst = new_def(f, s->dst);
            cur[s->dst.v] = s->dst;
        }
    }
    for (int k = 0; k < blk->nsucc; k++) {
        struct block *succ = &f->bb[blk->succ[k]];
        int at = 0;
        while (succ->pred[at] != b) {
            at++;
        }
        for (int i = 0; i < succ->nphi; i++) {
            struct stmt *phi = &succ->s[i];
            phi->args[at] = reaching_def(f, cur, phi->dst.v);
        }
    }
    for (int c = f->nbb - 1; c >= 2; c--) {
        if ((c != b) & (f->bb[c].idom == b)) {
            rename_block(f, c, cur);
        }
    }
}

static void stage_ssa(void)
{
    P("== stage_ssa: SSA form ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        compute_dominators(f);
        /* Which variables are live on entry to each block. */
        uint32_t ue[MAX_BLOCKS] = {0}, kill[MAX_BLOCKS] = {0}, defs[MAX_VARS] = {0};
        for (int b = 2; b < f->nbb; b++) {
            for (int j = 0; j < f->bb[b].ns; j++) {
                const struct stmt *s = &f->bb[b].s[j];
                ue[b] |= (vbit(s->a) | vbit(s->b)) & ~kill[b];
                kill[b] |= vbit(s->dst);
                for (int v = 0; v < f->nvars; v++) {
                    defs[v] |= (uint32_t)((vbit(s->dst) >> v) & 1u) << b;
                }
            }
        }
        for (int changed = 1; changed;) {
            changed = 0;
            for (int b = f->nbb - 1; b >= 2; b--) {
                uint32_t out = 0;
                for (int k = 0; k < f->bb[b].nsucc; k++) {
                    out |= f->bb[f->bb[b].succ[k]].livein;
                }
                const uint32_t in = ue[b] | (out & ~kill[b]);
                changed |= in != f->bb[b].livein;
                f->bb[b].livein = in;
            }
        }
        for (int v = 0; v < f->nvars; v++) {
            uint32_t phis = 0, work = defs[v];
            while (work != 0) {
                int b = 2;
                while (((work >> b) & 1u) == 0) {
                    b++;
                }
                work &= ~(1u << b);
                const uint32_t added = f->bb[b].df & ~phis;
                phis |= added;
                work |= added & ~defs[v];
            }
            for (int b = 2; b < f->nbb; b++) {
                struct block *blk = &f->bb[b];
                if (((phis >> b) & 1u) & ((blk->livein >> v) & 1u)) {
                    memmove(&blk->s[blk->nphi + 1], &blk->s[blk->nphi],
                            (size_t)(blk->ns - blk->nphi) * sizeof blk->s[0]);
                    blk->s[blk->nphi] = (struct stmt){.k = S_PHI, .nargs = blk->npred};
                    /* The result of a PHI is named when the PHI is made, before renaming. */
                    blk->s[blk->nphi].dst =
                        new_def(f, (struct opd){O_VAR, v, 0, 0, f->vars[v].name});
                    blk->nphi++;
                    blk->ns++;
                }
            }
        }
        struct opd none[MAX_VARS] = {{0}};
        rename_block(f, 2, none);
        print_cfg(f, 2);
    }
}

/* ------------------------------------------------------------------------------------------
 * RTL printing, in the format of -fdump-rtl-expand-slim.
 * ---------------------------------------------------------------------------------------- */

static void fmt_reg(char *buf, size_t n, int r, const char *mode)
{
    if (r < FIRST_PSEUDO) {
        snprintf(buf, n, "%s:%s", reg_short[r], mode);
    } else {
        snprintf(buf, n, "r%d:%s", r, mode);
    }
}

static void fmt_rx(char *buf, size_t n, struct rx x)
{
    if (x.k == X_IMM) {
        snprintf(buf, n, "0x%x", (unsigned)x.v);
    } else {
        fmt_reg(buf, n, x.v, "SI");
    }
}

static void fmt_rtl(char *buf, size_t n, const struct insn *in)
{
    char d[24], a[24], b[24];
    fmt_reg(d, sizeof d, in->dst < 0 ? AX : in->dst, "SI");
    fmt_rx(a, sizeof a, in->a);
    fmt_rx(b, sizeof b, in->b);
    switch (in->code) {
    case R_MOVE:
        snprintf(buf, n, "%s=%s", d, a);
        break;
    case R_ADD:
        if ((in->b.k == X_IMM) & (in->b.v < 0)) {
            snprintf(buf, n, "{%s=%s-0x%x;clobber flags:CC;}", d, a, (unsigned)-in->b.v);
        } else {
            snprintf(buf, n, "{%s=%s+%s;clobber flags:CC;}", d, a, b);
        }
        break;
    case R_MUL:
        snprintf(buf, n, "{%s=%s*%s;clobber flags:CC;}", d, a, b);
        break;
    case R_CMP:
        snprintf(buf, n, "flags:CCGC=cmp(%s,%s)", a, b);
        break;
    case R_JCC:
        snprintf(buf, n, "pc={(flags:CCGC%s0)?L%d:pc}", cmp_names[in->cond], in->target);
        break;
    case R_JMP:
        snprintf(buf, n, "pc=L%d", in->target);
        break;
    case R_CALL:
        if (in->dst >= 0) {
            snprintf(buf, n, "ax:SI=call [`%s'] argc:0", in->sym);
        } else {
            snprintf(buf, n, "call [`%s'] argc:0", in->sym);
        }
        break;
    case R_USE:
        snprintf(buf, n, "use %s", a);
        break;
    default: /* R_LEA_SYM */
        snprintf(buf, n, "di:DI=`%s'", in->sym);
        break;
    }
}

static void print_rtl(const struct func *f)
{
    int used[MAX_BLOCKS] = {0};
    for (int i = 0; i < f->nins; i++) {
        const struct insn *in = &f->ins[i];
        used[in->target] |= (!in->dead) & ((in->code == R_JCC) | (in->code == R_JMP));
    }
    P("%s\n", f->name);
    for (int i = 0; i < f->nins; i++) {
        const struct insn *in = &f->ins[i];
        char text[96];
        if (in->code == R_LABEL) {
            P(";; basic block %d\n", in->bb);
            if (used[in->bb]) {
                P("L%d:\n", in->bb);
            }
        } else if (!in->dead) {
            fmt_rtl(text, sizeof text, in);
            P("%4d: %s\n", in->uid, text);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_expand
 *
 * Out of SSA: the names that a PHI joins (its result and its arguments) are put in one
 * partition, and every partition gets one pseudo register (tree-outof-ssa.c, which merges the
 * partitions of coalescable names). GCC then also builds the copies that a PHI needs on every
 * edge (eliminate_phi); here the partitions of the examples never interfere, so no copy is
 * needed and that part is left out. Pseudos are numbered from 98 in order of the lowest name in
 * the partition.
 *
 * Expanding a statement follows cfgexpand.c: a parameter is copied out of its hard register,
 * a call moves its argument to di and its result out of ax, a return moves the value to ax and
 * uses it, and a condition becomes a compare and a conditional jump (expand_gimple_cond): if
 * the false block is next, jump to the true block, else if the true block is next, jump on the
 * inverse condition to the false block.
 * ---------------------------------------------------------------------------------------- */

static int find(const int *parent, int x)
{
    while (parent[x] != x) {
        x = parent[x];
    }
    return x;
}

static void add_insn(struct func *f, struct insn in)
{
    in.uid = f->next_uid++;
    f->ins[f->nins++] = in;
}

static struct rx rx_of(const struct func *f, const int *parent, struct opd o)
{
    if (o.k == O_CONST) {
        return (struct rx){X_IMM, o.v};
    }
    return (struct rx){X_REG, f->preg[find(parent, o.ver)]};
}

static void stage_expand(void)
{
    P("== stage_expand: RTL ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        int parent[MAX_VERS], exists[MAX_VERS] = {0};
        for (int v = 0; v < MAX_VERS; v++) {
            parent[v] = v;
        }
        for (int b = 2; b < f->nbb; b++) {
            for (int j = 0; j < f->bb[b].ns; j++) {
                const struct stmt *s = &f->bb[b].s[j];
                exists[s->dst.ver] |= s->dst.k == O_SSA;
                exists[s->a.ver] |= s->a.k == O_SSA;
                exists[s->b.ver] |= s->b.k == O_SSA;
                for (int k = 0; k < s->nargs; k++) {
                    exists[s->args[k].ver] = 1;
                    parent[find(parent, s->args[k].ver)] = find(parent, s->dst.ver);
                }
            }
        }
        f->next_pseudo = FIRST_PSEUDO;
        for (int v = 1; v < MAX_VERS; v++) {
            if (exists[v] & (f->preg[find(parent, v)] == 0)) {
                f->preg[find(parent, v)] = f->next_pseudo++;
            }
        }
        P("partitions of %s:", f->name);
        for (int v = 1; v < MAX_VERS; v++) {
            if (exists[v]) {
                P(" %d:r%d", v, f->preg[find(parent, v)]);
            }
        }
        P("\n");
        for (int b = 2; b < f->nbb; b++) {
            const struct block *blk = &f->bb[b];
            add_insn(f, (struct insn){.code = R_LABEL, .bb = b});
            if ((b == 2) & (f->nparams > 0)) {
                const struct opd *p = &f->deflt[0];
                add_insn(f, (struct insn){.code = R_MOVE, .bb = b,
                                          .dst = f->preg[find(parent, p->ver)],
                                          .a = {X_REG, DI}});
            }
            for (int j = blk->nphi; j < blk->ns; j++) {
                const struct stmt *s = &blk->s[j];
                struct insn in = {.bb = b};
                switch (s->k) {
                case S_ASSIGN: {
                    struct rx x = rx_of(f, parent, s->a);
                    struct rx y = rx_of(f, parent, s->b);
                    in.dst = f->preg[find(parent, s->dst.ver)];
                    if (s->bop == B_NONE) {
                        in.code = R_MOVE;
                        in.a = x;
                    } else {
                        /* A constant goes second, as tree_swap_operands_p wants. */
                        in.code = s->bop == B_ADD ? R_ADD : R_MUL;
                        in.a = x;
                        in.b = y;
                    }
                    add_insn(f, in);
                    break;
                }
                case S_CALL:
                    if (s->a.k == O_STR) {
                        add_insn(f, (struct insn){.code = R_LEA_SYM, .bb = b, .dst = DI,
                                                  .sym = ".LC0"});
                    } else {
                        add_insn(f, (struct insn){.code = R_MOVE, .bb = b, .dst = DI,
                                                  .a = rx_of(f, parent, s->a)});
                    }
                    add_insn(f, (struct insn){.code = R_CALL, .bb = b, .has_arg = 1,
                                              .dst = s->dst.k == O_NONE ? -1 : AX,
                                              .sym = s->fn});
                    if (s->dst.k != O_NONE) {
                        add_insn(f, (struct insn){.code = R_MOVE, .bb = b,
                                                  .dst = f->preg[find(parent, s->dst.ver)],
                                                  .a = {X_REG, AX}});
                    }
                    break;
                case S_COND: {
                    const int next = b + 1;
                    const int t = s->l1, e = s->l2;
                    struct insn cmp = {.code = R_CMP, .bb = b, .a = rx_of(f, parent, s->a),
                                       .b = rx_of(f, parent, s->b)};
                    require(cmp.a.k == X_REG, "a compare whose first operand is a constant");
                    require((e == next) | (t == next), "a condition with no next target");
                    add_insn(f, cmp);
                    if (e == next) {
                        add_insn(f, (struct insn){.code = R_JCC, .bb = b, .cond = s->cmp,
                                                  .target = t});
                    } else {
                        add_insn(f, (struct insn){.code = R_JCC, .bb = b,
                                                  .cond = cmp_inverse[s->cmp], .target = e});
                    }
                    break;
                }
                default: /* S_RETURN */
                    if (s->a.k != O_NONE) {
                        add_insn(f, (struct insn){.code = R_MOVE, .bb = b, .dst = AX,
                                                  .a = rx_of(f, parent, s->a)});
                        add_insn(f, (struct insn){.code = R_USE, .bb = b, .a = {X_REG, AX}});
                    }
                    break;
                }
            }
            if ((blk->nsucc == 1) & (blk->succ[0] != b + 1)) {
                add_insn(f, (struct insn){.code = R_JMP, .bb = b, .target = blk->succ[0]});
            }
        }
        print_rtl(f);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_combine
 *
 * combine.c first runs make_more_copies: after every `pseudo = hard register` copy it makes a
 * new pseudo, so that `r101 = di` becomes `r104 = di; r101 = r104` and the hard register is
 * not tied to a long live range. Then try_combine substitutes the source of an insn into the
 * one insn that uses its result, and keeps the merge if the replacement is recognized and is
 * not more expensive than the pair. Only the case that the examples need is kept: a copy of a
 * pseudo into a pseudo that has one use, later in the same block. Every substitution of a
 * register for a register is recognized by the insn patterns, so the recognition step is not
 * a stage here. GCC does not combine into a move whose destination is a hard register, and
 * neither does this entry.
 * ---------------------------------------------------------------------------------------- */

static int reg_uses(const struct insn *in, int *out)
{
    int n = 0;
    switch (in->code) {
    case R_MOVE:
    case R_USE:
        out[n] = in->a.v;
        n += in->a.k == X_REG;
        break;
    case R_ADD:
    case R_MUL:
    case R_CMP:
        out[n] = in->a.v;
        n += in->a.k == X_REG;
        out[n] = in->b.v;
        n += in->b.k == X_REG;
        break;
    case R_CALL:
        out[n++] = DI;
        break;
    default: /* labels, jumps and lea use no register */
        break;
    }
    return n;
}

/* Everything a call clobbers is listed as defined. */
static int reg_defs(const struct insn *in, int *out)
{
    int n = 0;
    switch (in->code) {
    case R_MOVE:
    case R_ADD:
    case R_MUL:
    case R_LEA_SYM:
        out[n++] = in->dst;
        break;
    case R_CALL:
        out[n++] = AX;
        out[n++] = DX;
        out[n++] = CX;
        out[n++] = SI;
        out[n++] = DI;
        break;
    default: /* labels, compares, jumps and uses define no register */
        break;
    }
    return n;
}

static void insert_before(struct func *f, int at, struct insn in)
{
    memmove(&f->ins[at + 1], &f->ins[at], (size_t)(f->nins - at) * sizeof f->ins[0]);
    in.uid = f->next_uid++;
    f->ins[at] = in;
    f->nins++;
}

static void stage_combine(void)
{
    P("== stage_combine: RTL after combine ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        for (int j = 0; j < f->nins; j++) {
            struct insn *in = &f->ins[j];
            if ((in->code == R_MOVE) & (in->dst >= FIRST_PSEUDO) & (in->a.k == X_REG) &
                (in->a.v < FIRST_PSEUDO)) {
                const int fresh = f->next_pseudo++;
                struct insn copy = {.code = R_MOVE, .bb = in->bb, .dst = fresh, .a = in->a};
                in->a.v = fresh;
                insert_before(f, j, copy);
                j++;
            }
        }
        for (int j = 0; j < f->nins; j++) {
            const struct insn *i2 = &f->ins[j];
            int uses[3], n_use = 0, use_at = -1, n_def = 0;
            if (!((i2->code == R_MOVE) & (i2->dst >= FIRST_PSEUDO) & (i2->a.k == X_REG) &
                  (i2->a.v >= FIRST_PSEUDO))) {
                continue;
            }
            for (int k = 0; k < f->nins; k++) {
                int defs[5];
                const int nd = reg_defs(&f->ins[k], defs);
                for (int d = 0; d < nd; d++) {
                    n_def += defs[d] == i2->dst;
                }
                const int nu = reg_uses(&f->ins[k], uses);
                for (int u = 0; u < nu; u++) {
                    if (uses[u] == i2->dst) {
                        n_use++;
                        use_at = k;
                    }
                }
            }
            /* One definition, and one use in the very next insn, which is not a move into a hard
             * register. GCC also looks a few insns ahead. */
            if ((n_def != 1) | (n_use != 1) | (use_at != j + 1)) {
                continue;
            }
            struct insn *i3 = &f->ins[use_at];
            if ((i3->bb != i2->bb) | ((i3->code == R_MOVE) & (i3->dst < FIRST_PSEUDO))) {
                continue;
            }
            P("allowing combination of insns %d and %d, original costs %d + %d = %d, "
              "replacement cost %d\n",
              i2->uid, i3->uid, insn_cost[i2->code], insn_cost[i3->code],
              insn_cost[i2->code] + insn_cost[i3->code], insn_cost[i3->code]);
            require((i3->b.k == X_REG) & (i3->b.v == i2->dst), "a use in the first operand");
            i3->b = i2->a;
            f->ins[j].dead = 1;
        }
        print_rtl(f);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_ira
 *
 * ira-color.c colors a conflict graph. Two registers conflict if one is defined while the other
 * is live, except that the two sides of a copy do not. A call defines (clobbers) every
 * caller-saved register, so a pseudo that lives across a call conflicts with all of them and is
 * pushed into the one callee-saved register, bx. The coloring follows push_allocnos_to_stack
 * and pop_allocnos_from_stack: nodes with fewer neighbors than registers are pushed first, then
 * popped in reverse order, and each takes the cheapest hard register, where a copy to a
 * register that it already has or a hard register is a gain (assign_hard_reg preferences) and
 * the first use of a callee-saved register costs a save. There is no spilling, no live range
 * splitting and no coalescing apart from these preferences.
 * ---------------------------------------------------------------------------------------- */

static void stage_ira(void)
{
    P("== stage_ira: hard registers ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        uint8_t in_live[MAX_BLOCKS][MAX_REGS] = {{0}};
        uint8_t conf[MAX_REGS][MAX_REGS] = {{0}};
        int pairs[16][2], npairs = 0, used[MAX_REGS] = {0};
        /* Liveness of every register, hard or pseudo, to a fixed point. */
        for (int changed = 1; changed;) {
            changed = 0;
            for (int b = f->nbb - 1; b >= 2; b--) {
                uint8_t live[MAX_REGS] = {0};
                for (int k = 0; k < f->bb[b].nsucc; k++) {
                    for (int r = 0; r < MAX_REGS; r++) {
                        live[r] |= in_live[f->bb[b].succ[k]][r];
                    }
                }
                for (int j = f->nins - 1; j >= 0; j--) {
                    int regs[5];
                    const struct insn *in = &f->ins[j];
                    if ((in->bb != b) | in->dead) {
                        continue;
                    }
                    const int nd = reg_defs(in, regs);
                    for (int d = 0; d < nd; d++) {
                        live[regs[d]] = 0;
                    }
                    const int nu = reg_uses(in, regs);
                    for (int u = 0; u < nu; u++) {
                        live[regs[u]] = 1;
                    }
                }
                changed |= memcmp(live, in_live[b], sizeof live) != 0;
                memcpy(in_live[b], live, sizeof live);
            }
        }
        /* Conflicts and copy preferences, walking each block backward. */
        for (int b = 2; b < f->nbb; b++) {
            uint8_t live[MAX_REGS] = {0};
            for (int k = 0; k < f->bb[b].nsucc; k++) {
                for (int r = 0; r < MAX_REGS; r++) {
                    live[r] |= in_live[f->bb[b].succ[k]][r];
                }
            }
            for (int j = f->nins - 1; j >= 0; j--) {
                int regs[5];
                const struct insn *in = &f->ins[j];
                if ((in->bb != b) | in->dead) {
                    continue;
                }
                const int nd = reg_defs(in, regs);
                for (int d = 0; d < nd; d++) {
                    const int skip = ((in->code == R_MOVE) & (in->a.k == X_REG)) ? in->a.v : -1;
                    for (int r = 0; r < MAX_REGS; r++) {
                        if (live[r] & (r != regs[d]) & (r != skip)) {
                            conf[regs[d]][r] = 1;
                            conf[r][regs[d]] = 1;
                        }
                    }
                    used[regs[d]] = 1;
                }
                if ((in->code == R_MOVE) | (in->code == R_ADD) | (in->code == R_MUL)) {
                    if (in->a.k == X_REG) {
                        pairs[npairs][0] = in->dst;
                        pairs[npairs++][1] = in->a.v;
                    }
                }
                for (int d = 0; d < nd; d++) {
                    live[regs[d]] = 0;
                }
                const int nu = reg_uses(in, regs);
                for (int u = 0; u < nu; u++) {
                    live[regs[u]] = 1;
                    used[regs[u]] = 1;
                }
            }
        }
        /* Simplify: push the node with the fewest neighbors, as long as it has fewer than
         * the number of hard registers. */
        int stack[MAX_REGS], nstack = 0, pushed[MAX_REGS] = {0}, forbid[MAX_REGS] = {0};
        int npseudo = 0;
        for (int r = FIRST_PSEUDO; r < MAX_REGS; r++) {
            npseudo += used[r];
            for (int h = 0; h < NHARD; h++) {
                forbid[r] |= conf[r][h] << h;
            }
        }
        while (nstack < npseudo) {
            int best = -1, best_degree = NHARD;
            for (int r = FIRST_PSEUDO; r < MAX_REGS; r++) {
                int degree = 0;
                for (int h = 0; h < NHARD; h++) {
                    degree += (forbid[r] >> h) & 1;
                }
                for (int q = FIRST_PSEUDO; q < MAX_REGS; q++) {
                    degree += conf[r][q] & !pushed[q];
                }
                if (used[r] & !pushed[r] & (degree < best_degree)) {
                    best = r;
                    best_degree = degree;
                }
            }
            require(best >= 0, "a spill, which this entry has no code for");
            pushed[best] = 1;
            stack[nstack++] = best;
        }
        /* Select: pop in reverse order and give each node its cheapest allowed register. */
        int bx_used = 0;
        for (int s = nstack - 1; s >= 0; s--) {
            const int r = stack[s];
            int allowed = ((1 << NHARD) - 1) & ~forbid[r];
            for (int q = FIRST_PSEUDO; q < MAX_REGS; q++) {
                if (conf[r][q] & (f->assign[q] > 0)) {
                    allowed &= ~(1 << (f->assign[q] - 1));
                }
            }
            int best = -1, best_cost = 99;
            for (int o = 0; o < NHARD; o++) {
                const int h = alloc_order[o];
                int cost = (h == BX) & !bx_used;
                for (int p = 0; p < npairs; p++) {
                    const int other = pairs[p][0] == r ? pairs[p][1] : pairs[p][0];
                    const int mine = (pairs[p][0] == r) | (pairs[p][1] == r);
                    const int hard = other < FIRST_PSEUDO ? other : f->assign[other] - 1;
                    cost -= mine & (hard == h);
                }
                if (((allowed >> h) & 1) & (cost < best_cost)) {
                    best = h;
                    best_cost = cost;
                }
            }
            require(best >= 0, "a node with no register left");
            f->assign[r] = best + 1; /* 0 means not assigned yet */
            bx_used |= best == BX;
        }
        for (int r = FIRST_PSEUDO; r < MAX_REGS; r++) {
            if (used[r]) {
                P("r%d -> %s\n", r, reg_short[f->assign[r] - 1]);
            }
        }
        /* Rewrite the insns, and drop the copies whose two sides got the same register. */
        for (int j = 0; j < f->nins; j++) {
            struct insn *in = &f->ins[j];
            in->dst = in->dst >= FIRST_PSEUDO ? f->assign[in->dst] - 1 : in->dst;
            in->a.v = ((in->a.k == X_REG) & (in->a.v >= FIRST_PSEUDO)) ? f->assign[in->a.v] - 1
                                                                          : in->a.v;
            in->b.v = ((in->b.k == X_REG) & (in->b.v >= FIRST_PSEUDO)) ? f->assign[in->b.v] - 1
                                                                          : in->b.v;
            in->dead |= (in->code == R_MOVE) & (in->a.k == X_REG) & (in->a.v == in->dst);
        }
        print_rtl(f);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_frame
 *
 * thread_prologue_and_epilogue pushes every callee-saved register that the function uses. The
 * stack is 16-byte aligned at a call, and the return address already took 8 bytes, so a
 * function that makes calls and pushes an even number of registers reserves 8 more bytes.
 * There is no shrink-wrapping, which the real compiler does at -O1.
 * ---------------------------------------------------------------------------------------- */

static void stage_frame(void)
{
    P("== stage_frame: prologue and epilogue ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        for (int j = 0; j < f->nins; j++) {
            const struct insn *in = &f->ins[j];
            f->push_bx |= (!in->dead) & (((in->code != R_LABEL) & (in->dst == BX)) |
                                       ((in->a.k == X_REG) & (in->a.v == BX)));
            f->has_call |= in->code == R_CALL;
        }
        f->sub_rsp = f->has_call & !f->push_bx;
        P("%s: push rbx %d, sub rsp 8 %d\n", f->name, f->push_bx, f->sub_rsp);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_final
 *
 * final.c walks the insns and prints the output template of each. The templates of i386.md for
 * the insns here: a move is mov, an add of a constant is add if the destination is the first
 * operand and lea otherwise, and the two-address forms of add and imul copy the first operand
 * into the destination first. The assembly is AT&T syntax, as GCC prints it.
 * ---------------------------------------------------------------------------------------- */

enum {
    A_LABEL, A_PUSH, A_POP, A_MOV_RI, A_MOV_RR, A_LEA, A_LEA_RIP, A_ADD_RI, A_ADD_RR, A_IMUL,
    A_CMP_RI, A_CMP_RR, A_JCC, A_JMP, A_CALL, A_SUB_RSP, A_ADD_RSP, A_RET,
};
struct ainsn {
    int op, r1, r2, cond;
    int64_t imm;
    char sym[SYM];
};
static struct ainsn code[MAX_CODE];
static int ncode;
static int next_label = 1;

static void emit_a(struct ainsn a)
{
    code[ncode++] = a;
}

static void label_name(struct func *f, int bb, char *out)
{
    if (f->label_no[bb] == 0) {
        f->label_no[bb] = next_label++;
    }
    snprintf(out, SYM, ".L%d", f->label_no[bb]);
}

static void fmt_asm(char *buf, size_t n, const struct ainsn *a)
{
    static const char *const jcc[] = {"jl", "jle", "jge", "jg"};
    switch (a->op) {
    case A_LABEL:
        snprintf(buf, n, "%s:", a->sym);
        break;
    case A_PUSH:
        snprintf(buf, n, "pushq %%%s", reg64[a->r1]);
        break;
    case A_POP:
        snprintf(buf, n, "popq %%%s", reg64[a->r1]);
        break;
    case A_MOV_RI:
        snprintf(buf, n, "movl $%lld, %%%s", (long long)a->imm, reg32[a->r1]);
        break;
    case A_MOV_RR:
        snprintf(buf, n, "movl %%%s, %%%s", reg32[a->r2], reg32[a->r1]);
        break;
    case A_LEA:
        snprintf(buf, n, "leal %lld(%%%s), %%%s", (long long)a->imm, reg64[a->r2],
                 reg32[a->r1]);
        break;
    case A_LEA_RIP:
        snprintf(buf, n, "leaq %s(%%rip), %%%s", a->sym, reg64[a->r1]);
        break;
    case A_ADD_RI:
        snprintf(buf, n, "addl $%lld, %%%s", (long long)a->imm, reg32[a->r1]);
        break;
    case A_ADD_RR:
        snprintf(buf, n, "addl %%%s, %%%s", reg32[a->r2], reg32[a->r1]);
        break;
    case A_IMUL:
        snprintf(buf, n, "imull %%%s, %%%s", reg32[a->r2], reg32[a->r1]);
        break;
    case A_CMP_RI:
        snprintf(buf, n, "cmpl $%lld, %%%s", (long long)a->imm, reg32[a->r1]);
        break;
    case A_CMP_RR:
        snprintf(buf, n, "cmpl %%%s, %%%s", reg32[a->r2], reg32[a->r1]);
        break;
    case A_JCC:
        snprintf(buf, n, "%s %s", jcc[a->cond], a->sym);
        break;
    case A_JMP:
        snprintf(buf, n, "jmp %s", a->sym);
        break;
    case A_CALL:
        snprintf(buf, n, "call %s", a->sym);
        break;
    case A_SUB_RSP:
        snprintf(buf, n, "subq $%lld, %%rsp", (long long)a->imm);
        break;
    case A_ADD_RSP:
        snprintf(buf, n, "addq $%lld, %%rsp", (long long)a->imm);
        break;
    default: /* A_RET */
        snprintf(buf, n, "ret");
        break;
    }
}

static void emit_label(const char *name)
{
    struct ainsn a = {.op = A_LABEL};
    snprintf(a.sym, SYM, "%s", name);
    emit_a(a);
}

static void emit_sym(int op, int cond, const char *name)
{
    struct ainsn a = {.op = op, .cond = cond};
    snprintf(a.sym, SYM, "%s", name);
    emit_a(a);
}

/* Two-address form: the destination is the first operand, and add and imul commute, so the
 * operands swap if the destination is the second. IRA's preferences make one of them true. */
static void emit_two_address(int op, int dst, int x, int y)
{
    if (dst == y) {
        y = x;
        x = dst;
    }
    require(dst == x, "a two-address insn whose destination is not its first operand");
    emit_a((struct ainsn){.op = op, .r1 = dst, .r2 = y});
}

static void stage_final(void)
{
    P("== stage_final: assembly ==\n");
    for (int i = 0; i < nfuncs; i++) {
        struct func *f = &funcs[i];
        int used[MAX_BLOCKS] = {0};
        char name[SYM];
        for (int j = 0; j < f->nins; j++) {
            used[f->ins[j].target] |=
                (!f->ins[j].dead) & ((f->ins[j].code == R_JCC) | (f->ins[j].code == R_JMP));
        }
        emit_label(f->name);
        if (f->push_bx) {
            emit_a((struct ainsn){.op = A_PUSH, .r1 = BX});
        }
        if (f->sub_rsp) {
            emit_a((struct ainsn){.op = A_SUB_RSP, .imm = 8});
        }
        for (int j = 0; j < f->nins; j++) {
            const struct insn *in = &f->ins[j];
            if (in->dead) {
                continue;
            }
            switch (in->code) {
            case R_LABEL:
                if (used[in->bb]) {
                    label_name(f, in->bb, name);
                    emit_label(name);
                }
                break;
            case R_MOVE:
                if (in->a.k == X_IMM) {
                    emit_a((struct ainsn){.op = A_MOV_RI, .r1 = in->dst, .imm = in->a.v});
                } else {
                    emit_a((struct ainsn){.op = A_MOV_RR, .r1 = in->dst, .r2 = in->a.v});
                }
                break;
            case R_LEA_SYM:
                emit_sym(A_LEA_RIP, 0, in->sym);
                code[ncode - 1].r1 = in->dst;
                break;
            case R_ADD:
                if (in->b.k == X_IMM) {
                    if (in->dst == in->a.v) {
                        emit_a((struct ainsn){.op = A_ADD_RI, .r1 = in->dst, .imm = in->b.v});
                    } else {
                        emit_a((struct ainsn){.op = A_LEA, .r1 = in->dst, .r2 = in->a.v,
                                              .imm = in->b.v});
                    }
                } else {
                    emit_two_address(A_ADD_RR, in->dst, in->a.v, in->b.v);
                }
                break;
            case R_MUL:
                emit_two_address(A_IMUL, in->dst, in->a.v, in->b.v);
                break;
            case R_CMP:
                if (in->b.k == X_IMM) {
                    emit_a((struct ainsn){.op = A_CMP_RI, .r1 = in->a.v, .imm = in->b.v});
                } else {
                    emit_a((struct ainsn){.op = A_CMP_RR, .r1 = in->a.v, .r2 = in->b.v});
                }
                break;
            case R_JCC:
                label_name(f, in->target, name);
                emit_sym(A_JCC, in->cond, name);
                break;
            case R_JMP:
                label_name(f, in->target, name);
                emit_sym(A_JMP, 0, name);
                break;
            case R_CALL:
                emit_sym(A_CALL, 0, in->sym);
                break;
            default: /* R_USE needs no code */
                break;
            }
        }
        if (f->sub_rsp) {
            emit_a((struct ainsn){.op = A_ADD_RSP, .imm = 8});
        }
        if (f->push_bx) {
            emit_a((struct ainsn){.op = A_POP, .r1 = BX});
        }
        emit_a((struct ainsn){.op = A_RET});
    }
    for (int i = 0; i < ncode; i++) {
        char text[96];
        fmt_asm(text, sizeof text, &code[i]);
        P("%s%s\n", code[i].op == A_LABEL ? "" : "  ", text);
    }
    if (have_str) {
        P("  .section .rodata\n.LC0:\n  .string \"Hello, world!\\n\"\n");
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_encode
 *
 * GCC writes text and leaves the encoding to `as`, and the linking to `ld`. This stage does
 * both in one pass: every instruction becomes bytes, the label of each is recorded, and the
 * rel32 fields of jumps, calls and rip-relative addresses are patched at the end. The runtime
 * (print_int and print_str, which a real program takes from libc) and the string follow the
 * code in the same buffer.
 * ---------------------------------------------------------------------------------------- */

#define CODE_SIZE 4096
static uint8_t out[CODE_SIZE];
static size_t out_len;

struct label_def {
    char name[SYM];
    size_t off;
};
struct fixup {
    size_t at;
    char sym[SYM];
};
static struct label_def labels[MAX_LABELS * 2];
static int nlabels;
static struct fixup fixups[MAX_LABELS * 2];
static int nfixups;

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

static uint8_t imm8(int64_t v)
{
    require((v >= -128) & (v <= 127), "an immediate that needs 32 bits");
    return (uint8_t)v;
}

static void add_label(const char *name)
{
    snprintf(labels[nlabels].name, SYM, "%s", name);
    labels[nlabels++].off = out_len;
}

static size_t find_label(const char *name)
{
    int i = 0;
    while (strcmp(labels[i].name, name) != 0) {
        i++;
    }
    return labels[i].off;
}

static void emit_rel32(const char *sym)
{
    fixups[nfixups].at = out_len;
    snprintf(fixups[nfixups++].sym, SYM, "%s", sym);
    emit32(0);
}

static void emit_insn(const struct ainsn *in)
{
    const int r1 = hw_reg[in->r1];
    const int r2 = hw_reg[in->r2];
    static const uint8_t jcc_op[] = {0x8c, 0x8e, 0x8d, 0x8f};
    switch (in->op) {
    case A_LABEL:
        add_label(in->sym);
        break;
    case A_PUSH:
        EMIT((uint8_t)(0x50 + r1));
        break;
    case A_POP:
        EMIT((uint8_t)(0x58 + r1));
        break;
    case A_MOV_RI:
        EMIT((uint8_t)(0xb8 + r1));
        emit32((uint32_t)in->imm);
        break;
    case A_MOV_RR:
        EMIT(0x89, (uint8_t)(0xc0 | (r2 << 3) | r1));
        break;
    case A_LEA:
        EMIT(0x8d, (uint8_t)(0x40 | (r1 << 3) | r2), imm8(in->imm));
        break;
    case A_LEA_RIP:
        EMIT(0x48, 0x8d, (uint8_t)(0x05 | (r1 << 3)));
        emit_rel32(in->sym);
        break;
    case A_ADD_RI:
        EMIT(0x83, (uint8_t)(0xc0 | r1), imm8(in->imm));
        break;
    case A_ADD_RR:
        EMIT(0x01, (uint8_t)(0xc0 | (r2 << 3) | r1));
        break;
    case A_IMUL:
        EMIT(0x0f, 0xaf, (uint8_t)(0xc0 | (r1 << 3) | r2));
        break;
    case A_CMP_RI:
        EMIT(0x83, (uint8_t)(0xf8 | r1), imm8(in->imm));
        break;
    case A_CMP_RR:
        EMIT(0x39, (uint8_t)(0xc0 | (r2 << 3) | r1));
        break;
    case A_JCC:
        EMIT(0x0f, jcc_op[in->cond]);
        emit_rel32(in->sym);
        break;
    case A_JMP:
        EMIT(0xe9);
        emit_rel32(in->sym);
        break;
    case A_CALL:
        EMIT(0xe8);
        emit_rel32(in->sym);
        break;
    case A_SUB_RSP:
        EMIT(0x48, 0x83, 0xec, imm8(in->imm));
        break;
    case A_ADD_RSP:
        EMIT(0x48, 0x83, 0xc4, imm8(in->imm));
        break;
    default: /* A_RET */
        EMIT(0xc3);
        break;
    }
}

static void stage_encode(void)
{
    P("== stage_encode: machine code, with the offset of each instruction ==\n");
    for (int i = 0; i < ncode; i++) {
        char text[96];
        fmt_asm(text, sizeof text, &code[i]);
        P("  +0x%04zx  %s\n", out_len, text);
        emit_insn(&code[i]);
    }

    /* print_int(rdi): mov rax, rdi; then the digits are built backwards on the stack. */
    add_label("print_int");
    EMIT(0x48, 0x89, 0xf8, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x74, 0x24, 0x20,
         0x48, 0xc7, 0xc1, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xff, 0xce, 0xc6, 0x06, 0x0a,
         0x31, 0xd2, 0x48, 0xf7, 0xf1, 0x80, 0xc2, 0x30, 0x48, 0xff, 0xce, 0x88, 0x16,
         0x48, 0x85, 0xc0, 0x75, 0xee,
         0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0x29, 0xf2, 0xb8, 0x01, 0x00, 0x00, 0x00,
         0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x20, 0xc3);
    /* print_str(rdi): rdi points at a NUL-terminated string, whose length is found first. */
    add_label("print_str");
    EMIT(0x48, 0x89, 0xfe, 0x31, 0xd2, 0x80, 0x3c, 0x17, 0x00, 0x74, 0x05, 0x48, 0xff, 0xc2,
         0xeb, 0xf5, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05,
         0xc3);
    if (have_str) {
        add_label(".LC0");
        memcpy(&out[out_len], HELLO_STRING, sizeof HELLO_STRING);
        out_len += sizeof HELLO_STRING;
    }

    for (int i = 0; i < nfixups; i++) {
        const size_t target = find_label(fixups[i].sym);
        const uint32_t rel = (uint32_t)(int32_t)((int64_t)target - (int64_t)(fixups[i].at + 4));
        for (int j = 0; j < 4; j++) {
            out[fixups[i].at + (size_t)j] = (uint8_t)(rel >> (8 * j));
        }
        P("+0x%04zx  rel32 %d to %s at +0x%04zx\n", fixups[i].at, (int32_t)rel, fixups[i].sym,
          target);
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
    void *entry = (uint8_t *)buf + find_label("main");
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
    stage_ast(program);
    stage_gimplify();
    stage_lower();
    stage_cfg();
    stage_ssa();
    stage_expand();
    stage_combine();
    stage_ira();
    stage_frame();
    stage_final();
    stage_encode();
    stage_export(argc > 2 ? argv[2] : NULL);
    return 0;
}
