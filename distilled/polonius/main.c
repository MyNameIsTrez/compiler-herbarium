/*
 * polonius: a cut-down version of the Polonius borrow checker, a program analysis written as
 * Datalog rules over tables of facts.
 *
 * Most compilers in this repo compute with data structures and loops. Polonius computes with
 * relations. The compiler front end only writes down facts about the program, as rows of
 * tables: which points of the control-flow graph follow which, where a loan (a `&` or `&mut`
 * borrow) is issued, where a variable is used or assigned, which origins (the "lifetimes" of
 * references) flow into which. A handful of rules then derive everything else, and a generic
 * engine runs them until no rule produces a new row (semi-naive evaluation). Which loans are
 * still alive at a point, and whether an access conflicts with a live loan, are rows of the
 * output tables.
 *
 * This entry skips Rust, its type checker and MIR. Each example is lowered by hand into a
 * small control-flow graph of statements in which every read of a local variable takes a
 * shared borrow and every store to an initialized local takes a mutable borrow, so that the
 * examples contain loans. The facts, the liveness rules, the loan rules and the engine that
 * runs them are the real design in small. The borrow checker accepts or rejects the program;
 * an accepted program is then run by an interpreter, which is not part of Polonius.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr, followed by the same stages on a program the borrow checker rejects. No machine
 *   code is generated, so the code.bin argument is ignored.
 *
 * Stages (stage_facts, stage_liveness and stage_loans run once per function):
 *   stage_source    build the lowered program of the example and print it (stands in for
 *                   the front end and MIR building)
 *   stage_facts     write the input facts: cfg_edge, loan_issued_at, loan_killed_at,
 *                   loan_invalidated_at, subset_base, var_used_at, var_defined_at and others
 *   stage_liveness  derive which variables, then which origins, are live at each point
 *   stage_loans     derive subsets, the loans each origin contains, which loans are live,
 *                   and the errors
 *   stage_run       run an accepted program
 */
#include <stdio.h>
#include <string.h>

#define MAXV 24 /* variables per function */
#define MAXS 48 /* statements per function */
#define MAXF 2  /* functions per program */
#define MAXO 64 /* origins per function */
#define MAXL 32 /* loans per function */
#define MAXT 2048 /* rows per relation */
#define MAXB 4  /* atoms in a rule body */
#define NVAR 7  /* variables in a rule */

/* ------------------------------------------------------------------------------------------
 * The lowered program: statements over integer locals and references
 * ---------------------------------------------------------------------------------------- */
enum op {
    OP_CONST,     /* a = k */
    OP_ADD,       /* a = b + c */
    OP_SUB,       /* a = b - c */
    OP_MUL,       /* a = b * c */
    OP_BORROW,    /* a = &b (c = 0) or a = &mut b (c = 1) */
    OP_LOAD,      /* a = *b */
    OP_STORE,     /* *a = b */
    OP_COPY,      /* a = b, both references */
    OP_IFLT,      /* if b < c goto t */
    OP_GOTO,      /* goto t */
    OP_CALL,      /* a = f(b), where f is function k and b is a reference */
    OP_RET,       /* return a */
    OP_PRINT_INT, /* print b */
    OP_PRINT_STR  /* print s */
};

struct stmt {
    enum op op;
    int a, b, c, t;
    long k;
    const char *s;
};

struct func {
    const char *name;
    int nvars;
    const char *vname[MAXV];
    char buf[MAXV][8];
    int is_ref[MAXV];
    int param; /* the variable that holds the argument, or -1 */
    int n;
    struct stmt s[MAXS];
};

struct program {
    int nfuncs;
    struct func f[MAXF];
};

static struct program prog;
static struct program rejected;

static int newvar(struct func *f, char prefix, int is_ref)
{
    int v = f->nvars++;
    snprintf(f->buf[v], sizeof f->buf[v], "%c%d", prefix, v);
    f->vname[v] = f->buf[v];
    f->is_ref[v] = is_ref;
    return v;
}

static int named(struct func *f, const char *name, int is_ref)
{
    int v = f->nvars++;
    f->vname[v] = name;
    f->is_ref[v] = is_ref;
    return v;
}

static int emit(struct func *f, enum op op, int a, int b, int c, long k)
{
    int i = f->n++;
    f->s[i] = (struct stmt){op, a, b, c, 0, k, NULL};
    return i;
}

static struct func *new_func(struct program *p, const char *name)
{
    struct func *f = &p->f[p->nfuncs++];
    f->name = name;
    f->param = -1;
    return f;
}

/* a temporary that holds a constant */
static int cst(struct func *f, long k)
{
    int t = newvar(f, 't', 0);
    emit(f, OP_CONST, t, 0, 0, k);
    return t;
}

static int bin(struct func *f, enum op op, int x, int y)
{
    int t = newvar(f, 't', 0);
    emit(f, op, t, x, y, 0);
    return t;
}

/* Reading a local: borrow it, then load through the reference. */
static int rd(struct func *f, int local)
{
    int r = newvar(f, 'r', 1);
    emit(f, OP_BORROW, r, local, 0, 0);
    int t = newvar(f, 't', 0);
    emit(f, OP_LOAD, t, r, 0, 0);
    return t;
}

/* Storing to an initialized local: borrow it mutably, then store through the reference. */
static void wr(struct func *f, int local, int value)
{
    int r = newvar(f, 'r', 1);
    emit(f, OP_BORROW, r, local, 1, 0);
    emit(f, OP_STORE, r, value, 0, 0);
}

static void ret0(struct func *f)
{
    int t = cst(f, 0);
    emit(f, OP_RET, t, 0, 0, 0);
}

static void build_hello(void)
{
    struct func *m = new_func(&prog, "main");
    int i = emit(m, OP_PRINT_STR, 0, 0, 0, 0);
    m->s[i].s = "Hello, world!";
    ret0(m);
}

static void build_arithmetic(void)
{
    struct func *m = new_func(&prog, "main");
    int t1 = cst(m, 2);
    int t2 = cst(m, 3);
    int t3 = bin(m, OP_ADD, t1, t2);
    int t4 = cst(m, 4);
    int t5 = bin(m, OP_MUL, t3, t4);
    int t6 = cst(m, 6);
    int t7 = bin(m, OP_SUB, t5, t6);
    emit(m, OP_PRINT_INT, 0, t7, 0, 0);
    ret0(m);
}

static void build_branch(void)
{
    struct func *m = new_func(&prog, "main");
    int a = named(m, "a", 0);
    int b = named(m, "b", 0);
    emit(m, OP_CONST, a, 0, 0, 3);
    emit(m, OP_CONST, b, 0, 0, 5);
    int t1 = rd(m, a);
    int t2 = rd(m, b);
    int jlt = emit(m, OP_IFLT, 0, t1, t2, 0);
    int t3 = rd(m, a);
    emit(m, OP_PRINT_INT, 0, t3, 0, 0);
    int jend = emit(m, OP_GOTO, 0, 0, 0, 0);
    m->s[jlt].t = m->n;
    int t4 = rd(m, b);
    emit(m, OP_PRINT_INT, 0, t4, 0, 0);
    m->s[jend].t = m->n;
    ret0(m);
}

static void build_loop(void)
{
    struct func *m = new_func(&prog, "main");
    int sum = named(m, "sum", 0);
    int i = named(m, "i", 0);
    emit(m, OP_CONST, sum, 0, 0, 0);
    emit(m, OP_CONST, i, 0, 0, 1);
    int head = m->n;
    int t1 = rd(m, i);
    int t2 = cst(m, 11);
    int jlt = emit(m, OP_IFLT, 0, t1, t2, 0);
    int jend = emit(m, OP_GOTO, 0, 0, 0, 0);
    m->s[jlt].t = m->n;
    int t3 = rd(m, sum);
    int t4 = rd(m, i);
    int t5 = bin(m, OP_ADD, t3, t4);
    wr(m, sum, t5);
    int t6 = rd(m, i);
    int t7 = cst(m, 1);
    int t8 = bin(m, OP_ADD, t6, t7);
    wr(m, i, t8);
    int jback = emit(m, OP_GOTO, 0, 0, 0, 0);
    m->s[jback].t = head;
    m->s[jend].t = m->n;
    int t9 = rd(m, sum);
    emit(m, OP_PRINT_INT, 0, t9, 0, 0);
    ret0(m);
}

static void build_call(void)
{
    struct func *m = new_func(&prog, "main");
    struct func *f = new_func(&prog, "fact");
    int n = named(f, "n", 1);
    f->param = n;
    int t1 = newvar(f, 't', 0);
    emit(f, OP_LOAD, t1, n, 0, 0);
    int t2 = cst(f, 2);
    int jlt = emit(f, OP_IFLT, 0, t1, t2, 0);
    int t3 = newvar(f, 't', 0);
    emit(f, OP_LOAD, t3, n, 0, 0);
    int t4 = cst(f, 1);
    int t5 = bin(f, OP_SUB, t3, t4);
    int r = newvar(f, 'r', 1);
    emit(f, OP_BORROW, r, t5, 0, 0);
    int arg = newvar(f, 'a', 1);
    emit(f, OP_COPY, arg, r, 0, 0);
    int t6 = newvar(f, 't', 0);
    emit(f, OP_CALL, t6, arg, 0, 1);
    int t7 = newvar(f, 't', 0);
    emit(f, OP_LOAD, t7, n, 0, 0);
    int t8 = bin(f, OP_MUL, t7, t6);
    emit(f, OP_RET, t8, 0, 0, 0);
    f->s[jlt].t = f->n;
    int t9 = cst(f, 1);
    emit(f, OP_RET, t9, 0, 0, 0);

    int a = cst(m, 5);
    int mr = newvar(m, 'r', 1);
    emit(m, OP_BORROW, mr, a, 0, 0);
    int marg = newvar(m, 'a', 1);
    emit(m, OP_COPY, marg, mr, 0, 0);
    int res = newvar(m, 't', 0);
    emit(m, OP_CALL, res, marg, 0, 1);
    emit(m, OP_PRINT_INT, 0, res, 0, 0);
    ret0(m);
}

/* The program the borrow checker rejects: x is assigned while s, a copy of the reference r,
 * still holds the loan of x. */
static void build_rejected(void)
{
    struct func *m = new_func(&rejected, "main");
    int x = named(m, "x", 0);
    int r = named(m, "r", 1);
    int s = named(m, "s", 1);
    emit(m, OP_CONST, x, 0, 0, 1);
    emit(m, OP_BORROW, r, x, 0, 0);
    emit(m, OP_COPY, s, r, 0, 0);
    int t1 = newvar(m, 't', 0);
    emit(m, OP_LOAD, t1, r, 0, 0);
    emit(m, OP_CONST, x, 0, 0, 2);
    int t2 = newvar(m, 't', 0);
    emit(m, OP_LOAD, t2, s, 0, 0);
    emit(m, OP_PRINT_INT, 0, t2, 0, 0);
    ret0(m);
}

static void print_stmt(const struct program *p, const struct func *f, int i)
{
    const struct stmt *s = &f->s[i];
    const char **v = (const char **)f->vname;
    fprintf(stderr, "  %2d: ", i);
    switch (s->op) {
    case OP_CONST:
        fprintf(stderr, "%s = %ld", v[s->a], s->k);
        break;
    case OP_ADD:
        fprintf(stderr, "%s = %s + %s", v[s->a], v[s->b], v[s->c]);
        break;
    case OP_SUB:
        fprintf(stderr, "%s = %s - %s", v[s->a], v[s->b], v[s->c]);
        break;
    case OP_MUL:
        fprintf(stderr, "%s = %s * %s", v[s->a], v[s->b], v[s->c]);
        break;
    case OP_BORROW:
        fprintf(stderr, "%s = &%s%s", v[s->a], s->c ? "mut " : "", v[s->b]);
        break;
    case OP_LOAD:
        fprintf(stderr, "%s = *%s", v[s->a], v[s->b]);
        break;
    case OP_STORE:
        fprintf(stderr, "*%s = %s", v[s->a], v[s->b]);
        break;
    case OP_COPY:
        fprintf(stderr, "%s = %s", v[s->a], v[s->b]);
        break;
    case OP_IFLT:
        fprintf(stderr, "if %s < %s goto %d", v[s->b], v[s->c], s->t);
        break;
    case OP_GOTO:
        fprintf(stderr, "goto %d", s->t);
        break;
    case OP_CALL:
        fprintf(stderr, "%s = %s(%s)", v[s->a], p->f[s->k].name, v[s->b]);
        break;
    case OP_RET:
        fprintf(stderr, "return %s", v[s->a]);
        break;
    case OP_PRINT_INT:
        fprintf(stderr, "print %s", v[s->b]);
        break;
    default: /* OP_PRINT_STR */
        fprintf(stderr, "print \"%s\"", s->s);
        break;
    }
    fputc('\n', stderr);
}

static void print_program(const struct program *p)
{
    for (int fi = 0; fi < p->nfuncs; fi++) {
        const struct func *f = &p->f[fi];
        fprintf(stderr, "fn %s(", f->name);
        if (f->param >= 0)
            fprintf(stderr, "%s: &", f->vname[f->param]);
        fprintf(stderr, ")\n");
        for (int i = 0; i < f->n; i++)
            print_stmt(p, f, i);
    }
}

static struct program *stage_source(const char *example)
{
    memset(&prog, 0, sizeof prog);
    if (strcmp(example, "hello") == 0)
        build_hello();
    else if (strcmp(example, "arithmetic") == 0)
        build_arithmetic();
    else if (strcmp(example, "branch") == 0)
        build_branch();
    else if (strcmp(example, "loop") == 0)
        build_loop();
    else if (strcmp(example, "call") == 0)
        build_call();
    else
        return NULL;
    fprintf(stderr, "== stage_source: lowered program of %s ==\n", example);
    print_program(&prog);
    return &prog;
}

/* ------------------------------------------------------------------------------------------
 * Relations and the engine that evaluates rules over them
 * ---------------------------------------------------------------------------------------- */
enum rel {
    /* input facts */
    CFG_EDGE,
    LOAN_ISSUED_AT,
    LOAN_KILLED_AT,
    LOAN_INVALIDATED_AT,
    SUBSET_BASE,
    PLACEHOLDER,
    VAR_USED_AT,
    VAR_DEFINED_AT,
    USE_OF_VAR_DEREFS_ORIGIN,
    POINT,
    /* derived by stage_liveness */
    VAR_LIVE_ON_ENTRY,
    ORIGIN_LIVE_ON_ENTRY,
    /* derived by stage_loans */
    SUBSET,
    ORIGIN_CONTAINS_LOAN_ON_ENTRY,
    LOAN_LIVE_AT,
    ERRORS,
    NREL
};

/* The columns of a relation: p point, v variable, o origin, l loan. The number of columns is
 * the arity. Every value is a small integer, as in Polonius, where every atom is interned. */
struct relation {
    const char *name;
    const char *cols;
    int n;      /* rows */
    int lo, hi; /* the rows added in the previous round are [lo, hi) */
    int t[MAXT][3];
};

static struct relation rels[NREL] = {
    [CFG_EDGE] = {.name = "cfg_edge", .cols = "pp"},
    [LOAN_ISSUED_AT] = {.name = "loan_issued_at", .cols = "olp"},
    [LOAN_KILLED_AT] = {.name = "loan_killed_at", .cols = "lp"},
    [LOAN_INVALIDATED_AT] = {.name = "loan_invalidated_at", .cols = "lp"},
    [SUBSET_BASE] = {.name = "subset_base", .cols = "oop"},
    [PLACEHOLDER] = {.name = "placeholder", .cols = "o"},
    [VAR_USED_AT] = {.name = "var_used_at", .cols = "vp"},
    [VAR_DEFINED_AT] = {.name = "var_defined_at", .cols = "vp"},
    [USE_OF_VAR_DEREFS_ORIGIN] = {.name = "use_of_var_derefs_origin", .cols = "vo"},
    [POINT] = {.name = "point", .cols = "p"},
    [VAR_LIVE_ON_ENTRY] = {.name = "var_live_on_entry", .cols = "vp"},
    [ORIGIN_LIVE_ON_ENTRY] = {.name = "origin_live_on_entry", .cols = "op"},
    [SUBSET] = {.name = "subset", .cols = "oop"},
    [ORIGIN_CONTAINS_LOAN_ON_ENTRY] = {.name = "origin_contains_loan_on_entry", .cols = "olp"},
    [LOAN_LIVE_AT] = {.name = "loan_live_at", .cols = "lp"},
    [ERRORS] = {.name = "errors", .cols = "lp"},
};

static int arity(int r)
{
    return (int)strlen(rels[r].cols);
}

/* Add a row unless it is already there. */
static void insert(int r, const int *row)
{
    struct relation *rl = &rels[r];
    for (int i = 0; i < rl->n; i++)
        if (memcmp(rl->t[i], row, arity(r) * sizeof(int)) == 0)
            return;
    memcpy(rl->t[rl->n++], row, arity(r) * sizeof(int));
}

static void add(int r, int a, int b, int c)
{
    int row[3] = {a, b, c};
    insert(r, row);
}

/* The facts of one function: the names the dump uses, and the loan table. */
static const struct func *cur;
static char oname[MAXO][12];
static int norigins;
static int var_origin[MAXV];
static struct {
    int var, mut, point, origin;
} loans[MAXL];
static int nloans;

static void name(char kind, int id)
{
    if (kind == 'p')
        fprintf(stderr, "%s:%d", cur->name, id);
    else if (kind == 'v')
        fprintf(stderr, "%s", cur->vname[id]);
    else if (kind == 'o')
        fprintf(stderr, "%s", oname[id]);
    else
        fprintf(stderr, "L%d", id);
}

static void print_rows(int r)
{
    struct relation *rl = &rels[r];
    fprintf(stderr, "%s (%d)", rl->name, rl->n);
    for (int i = 0; i < rl->n; i++) {
        fprintf(stderr, i % 4 == 0 ? "\n   " : " ");
        fputc('(', stderr);
        for (int j = 0; j < arity(r); j++) {
            if (j > 0)
                fputc(',', stderr);
            name(rl->cols[j], rl->t[i][j]);
        }
        fputc(')', stderr);
    }
    fputc('\n', stderr);
}

/* A rule is `head :- body`. Every argument is a rule variable, an index into a binding. A
 * negated atom only filters: all its variables are bound by the atoms before it. */
enum { O1, O2, O3, LN, P, Q, V };

struct atom {
    int rel;
    int v[3];
    int neg;
};

struct rule {
    const char *text;
    struct atom head;
    int nbody;
    struct atom body[MAXB];
};

struct binding {
    int val[NVAR];
    int set[NVAR];
};

static int unify(const struct atom *a, const int *row, struct binding *b)
{
    for (int i = 0; i < arity(a->rel); i++) {
        int v = a->v[i];
        if (!b->set[v]) {
            b->set[v] = 1;
            b->val[v] = row[i];
        } else if (b->val[v] != row[i]) {
            return 0;
        }
    }
    return 1;
}

/* Join the body of a rule from atom j on. The atom `delta` only sees the rows added in the
 * previous round, the others see every row up to the start of this round. */
static void join(const struct rule *rule, int delta, int j, struct binding b)
{
    if (j == rule->nbody) {
        int row[3] = {0, 0, 0};
        for (int i = 0; i < arity(rule->head.rel); i++)
            row[i] = b.val[rule->head.v[i]];
        insert(rule->head.rel, row);
        return;
    }
    const struct atom *a = &rule->body[j];
    struct relation *rl = &rels[a->rel];
    if (a->neg) {
        for (int k = 0; k < rl->n; k++) {
            struct binding c = b;
            if (unify(a, rl->t[k], &c))
                return;
        }
        join(rule, delta, j + 1, b);
        return;
    }
    for (int k = j == delta ? rl->lo : 0; k < rl->hi; k++) {
        struct binding c = b;
        if (unify(a, rl->t[k], &c))
            join(rule, delta, j + 1, c);
    }
}

/* Semi-naive evaluation. In every round each rule is run once per positive body atom, with
 * that atom restricted to the rows that are new since the last round. A rule can only give
 * a new row if one of its atoms matches a new row. The loop stops when a round adds nothing. */
static void solve(const struct rule *rules, int nrules)
{
    for (int r = 0; r < NREL; r++)
        rels[r].lo = 0;
    for (int round = 1;; round++) {
        int fresh = 0;
        for (int r = 0; r < NREL; r++) {
            rels[r].hi = rels[r].n;
            fresh |= rels[r].lo < rels[r].hi;
        }
        if (!fresh)
            break;
        for (int i = 0; i < nrules; i++)
            for (int j = 0; j < rules[i].nbody; j++)
                if (!rules[i].body[j].neg)
                    join(&rules[i], j, 0, (struct binding){{0}, {0}});
        fprintf(stderr, "  round %d:", round);
        int any = 0;
        for (int r = 0; r < NREL; r++) {
            if (rels[r].n > rels[r].hi) {
                fprintf(stderr, " %s +%d", rels[r].name, rels[r].n - rels[r].hi);
                any = 1;
            }
            rels[r].lo = rels[r].hi;
        }
        fprintf(stderr, any ? "\n" : " nothing new\n");
    }
}

#define POS(r, ...) {r, {__VA_ARGS__}, 0}
#define NEG(r, ...) {r, {__VA_ARGS__}, 1}

/* ------------------------------------------------------------------------------------------
 * Input facts
 * ---------------------------------------------------------------------------------------- */

/* An access to the local x at point p invalidates the loans of x that conflict with it: any
 * access conflicts with a mutable loan, only a write conflicts with a shared one. The loan
 * that p issues itself is not invalidated by p. */
static void access(int x, int write, int p)
{
    for (int l = 0; l < nloans; l++)
        if (loans[l].var == x && loans[l].point != p && (write || loans[l].mut))
            add(LOAN_INVALIDATED_AT, l, p, 0);
}

static void use(int x, int p)
{
    add(VAR_USED_AT, x, p, 0);
    access(x, 0, p);
}

/* Assigning x also kills its loans: the path they borrowed has been replaced. */
static void define(int x, int p)
{
    add(VAR_DEFINED_AT, x, p, 0);
    for (int l = 0; l < nloans; l++)
        if (loans[l].var == x)
            add(LOAN_KILLED_AT, l, p, 0);
    access(x, 1, p);
}

static void stage_facts(const struct func *f)
{
    for (int r = 0; r < NREL; r++)
        rels[r].n = 0;
    cur = f;
    norigins = 0;
    nloans = 0;

    /* every reference variable has an origin, the one in its type */
    for (int v = 0; v < f->nvars; v++)
        if (f->is_ref[v]) {
            var_origin[v] = norigins;
            snprintf(oname[norigins++], sizeof oname[0], "'%s", f->vname[v]);
        }
    /* every borrow issues a loan, and has an origin of its own, the one of the `&` */
    for (int i = 0; i < f->n; i++)
        if (f->s[i].op == OP_BORROW) {
            loans[nloans].var = f->s[i].b;
            loans[nloans].mut = f->s[i].c;
            loans[nloans].point = i;
            loans[nloans].origin = norigins;
            snprintf(oname[norigins++], sizeof oname[0], "'L%d", nloans);
            nloans++;
        }

    for (int v = 0; v < f->nvars; v++)
        if (f->is_ref[v])
            add(USE_OF_VAR_DEREFS_ORIGIN, v, var_origin[v], 0);
    if (f->param >= 0)
        add(PLACEHOLDER, var_origin[f->param], 0, 0);

    for (int p = 0; p < f->n; p++) {
        const struct stmt *s = &f->s[p];
        add(POINT, p, 0, 0);
        if (s->op == OP_IFLT || s->op == OP_GOTO)
            add(CFG_EDGE, p, s->t, 0);
        if (s->op != OP_GOTO && s->op != OP_RET)
            add(CFG_EDGE, p, p + 1, 0);
        switch (s->op) {
        case OP_CONST:
            define(s->a, p);
            break;
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
            use(s->b, p);
            use(s->c, p);
            define(s->a, p);
            break;
        case OP_BORROW: {
            int l = 0;
            while (loans[l].point != p)
                l++;
            access(s->b, s->c, p);
            add(LOAN_ISSUED_AT, loans[l].origin, l, p);
            add(SUBSET_BASE, loans[l].origin, var_origin[s->a], p);
            define(s->a, p);
            break;
        }
        case OP_LOAD:
            use(s->b, p);
            define(s->a, p);
            break;
        case OP_STORE:
            use(s->a, p);
            use(s->b, p);
            break;
        case OP_COPY:
            use(s->b, p);
            add(SUBSET_BASE, var_origin[s->b], var_origin[s->a], p);
            define(s->a, p);
            break;
        case OP_IFLT:
            use(s->b, p);
            use(s->c, p);
            break;
        case OP_CALL:
            use(s->b, p);
            define(s->a, p);
            break;
        case OP_RET:
            use(s->a, p);
            break;
        case OP_PRINT_INT:
            use(s->b, p);
            break;
        default: /* OP_GOTO and OP_PRINT_STR add no facts of their own */
            break;
        }
    }

    fprintf(stderr, "== stage_facts: input facts of fn %s ==\n", f->name);
    for (int i = 0; i < nloans; i++) {
        fprintf(stderr, "L%d = &%s%s at ", i, loans[i].mut ? "mut " : "", f->vname[loans[i].var]);
        name('p', loans[i].point);
        fprintf(stderr, ", issued into %s\n", oname[loans[i].origin]);
    }
    for (int r = 0; r < POINT; r++)
        print_rows(r);
    fprintf(stderr, "point (%d)\n", rels[POINT].n);
}

/* ------------------------------------------------------------------------------------------
 * Liveness: from variables to origins
 * ---------------------------------------------------------------------------------------- */
static void stage_liveness(void)
{
    static const struct rule rules[] = {
        {"var_live_on_entry(V, P) :- var_used_at(V, P).",
         POS(VAR_LIVE_ON_ENTRY, V, P),
         1,
         {POS(VAR_USED_AT, V, P)}},
        {"var_live_on_entry(V, P) :- var_live_on_entry(V, Q), cfg_edge(P, Q), !var_defined_at(V, P).",
         POS(VAR_LIVE_ON_ENTRY, V, P),
         3,
         {POS(VAR_LIVE_ON_ENTRY, V, Q), POS(CFG_EDGE, P, Q), NEG(VAR_DEFINED_AT, V, P)}},
        {"origin_live_on_entry(O1, P) :- var_live_on_entry(V, P), use_of_var_derefs_origin(V, O1).",
         POS(ORIGIN_LIVE_ON_ENTRY, O1, P),
         2,
         {POS(VAR_LIVE_ON_ENTRY, V, P), POS(USE_OF_VAR_DEREFS_ORIGIN, V, O1)}},
        {"origin_live_on_entry(O1, P) :- placeholder(O1), point(P).",
         POS(ORIGIN_LIVE_ON_ENTRY, O1, P),
         2,
         {POS(PLACEHOLDER, O1), POS(POINT, P)}},
    };
    fprintf(stderr, "== stage_liveness: liveness of fn %s ==\n", cur->name);
    for (int i = 0; i < 4; i++)
        fprintf(stderr, "  %s\n", rules[i].text);
    solve(rules, 4);
    print_rows(VAR_LIVE_ON_ENTRY);
    print_rows(ORIGIN_LIVE_ON_ENTRY);
}

/* ------------------------------------------------------------------------------------------
 * Loans: subsets, the loans in each origin, and the errors
 * ---------------------------------------------------------------------------------------- */
static void print_loan_summary(void)
{
    for (int l = 0; l < nloans; l++) {
        fprintf(stderr, "  L%d (&%s%s at ", l, loans[l].mut ? "mut " : "", cur->vname[loans[l].var]);
        name('p', loans[l].point);
        fprintf(stderr, ")");
        const char *what[] = {"live at", "invalidated at", "error at"};
        const int from[] = {LOAN_LIVE_AT, LOAN_INVALIDATED_AT, ERRORS};
        for (int k = 0; k < 3; k++) {
            const struct relation *rl = &rels[from[k]];
            int first = 1;
            for (int i = 0; i < rl->n; i++)
                if (rl->t[i][0] == l) {
                    fprintf(stderr, first ? "; %s " : ", ", what[k]);
                    name('p', rl->t[i][1]);
                    first = 0;
                }
        }
        fputc('\n', stderr);
    }
}

static int stage_loans(void)
{
    static const struct rule rules[] = {
        {"R1 subset(O1, O2, P) :- subset_base(O1, O2, P).",
         POS(SUBSET, O1, O2, P),
         1,
         {POS(SUBSET_BASE, O1, O2, P)}},
        {"R2 subset(O1, O3, P) :- subset(O1, O2, P), subset(O2, O3, P).",
         POS(SUBSET, O1, O3, P),
         2,
         {POS(SUBSET, O1, O2, P), POS(SUBSET, O2, O3, P)}},
        {"R3 subset(O1, O2, Q) :- subset(O1, O2, P), cfg_edge(P, Q), origin_live_on_entry(O1, Q), "
         "origin_live_on_entry(O2, Q).",
         POS(SUBSET, O1, O2, Q),
         4,
         {POS(SUBSET, O1, O2, P), POS(CFG_EDGE, P, Q), POS(ORIGIN_LIVE_ON_ENTRY, O1, Q),
          POS(ORIGIN_LIVE_ON_ENTRY, O2, Q)}},
        {"R4 origin_contains_loan_on_entry(O1, L, P) :- loan_issued_at(O1, L, P).",
         POS(ORIGIN_CONTAINS_LOAN_ON_ENTRY, O1, LN, P),
         1,
         {POS(LOAN_ISSUED_AT, O1, LN, P)}},
        {"R5 origin_contains_loan_on_entry(O2, L, P) :- origin_contains_loan_on_entry(O1, L, P), "
         "subset(O1, O2, P).",
         POS(ORIGIN_CONTAINS_LOAN_ON_ENTRY, O2, LN, P),
         2,
         {POS(ORIGIN_CONTAINS_LOAN_ON_ENTRY, O1, LN, P), POS(SUBSET, O1, O2, P)}},
        {"R6 origin_contains_loan_on_entry(O1, L, Q) :- origin_contains_loan_on_entry(O1, L, P), "
         "!loan_killed_at(L, P), cfg_edge(P, Q), origin_live_on_entry(O1, Q).",
         POS(ORIGIN_CONTAINS_LOAN_ON_ENTRY, O1, LN, Q),
         4,
         {POS(ORIGIN_CONTAINS_LOAN_ON_ENTRY, O1, LN, P), NEG(LOAN_KILLED_AT, LN, P),
          POS(CFG_EDGE, P, Q), POS(ORIGIN_LIVE_ON_ENTRY, O1, Q)}},
        {"R7 loan_live_at(L, P) :- origin_contains_loan_on_entry(O1, L, P), origin_live_on_entry(O1, P).",
         POS(LOAN_LIVE_AT, LN, P),
         2,
         {POS(ORIGIN_CONTAINS_LOAN_ON_ENTRY, O1, LN, P), POS(ORIGIN_LIVE_ON_ENTRY, O1, P)}},
        {"R8 errors(L, P) :- loan_invalidated_at(L, P), loan_live_at(L, P).",
         POS(ERRORS, LN, P),
         2,
         {POS(LOAN_INVALIDATED_AT, LN, P), POS(LOAN_LIVE_AT, LN, P)}},
    };
    fprintf(stderr, "== stage_loans: loan analysis of fn %s ==\n", cur->name);
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "  %s\n", rules[i].text);
    solve(rules, 8);
    print_rows(SUBSET);
    print_rows(ORIGIN_CONTAINS_LOAN_ON_ENTRY);
    print_loan_summary();
    print_rows(ERRORS);
    return rels[ERRORS].n;
}

/* ------------------------------------------------------------------------------------------
 * The interpreter for an accepted program (not part of Polonius)
 * ---------------------------------------------------------------------------------------- */
static long mem[256];
static int sp;
static long steps;

/* A reference is the index of a slot in mem. */
static long call_fn(const struct program *p, int fi, long arg, int depth)
{
    const struct func *f = &p->f[fi];
    int base = sp;
    sp += f->nvars;
    if (f->param >= 0)
        mem[base + f->param] = arg;
    for (int pc = 0;;) {
        const struct stmt *s = &f->s[pc];
        long *a = &mem[base + s->a];
        steps++;
        pc++;
        switch (s->op) {
        case OP_CONST:
            *a = s->k;
            break;
        case OP_ADD:
            *a = mem[base + s->b] + mem[base + s->c];
            break;
        case OP_SUB:
            *a = mem[base + s->b] - mem[base + s->c];
            break;
        case OP_MUL:
            *a = mem[base + s->b] * mem[base + s->c];
            break;
        case OP_BORROW:
            *a = base + s->b;
            break;
        case OP_LOAD:
            *a = mem[mem[base + s->b]];
            break;
        case OP_STORE:
            mem[*a] = mem[base + s->b];
            break;
        case OP_COPY:
            *a = mem[base + s->b];
            break;
        case OP_IFLT:
            if (mem[base + s->b] < mem[base + s->c])
                pc = s->t;
            break;
        case OP_GOTO:
            pc = s->t;
            break;
        case OP_CALL: {
            long ref = mem[base + s->b];
            fprintf(stderr, "  %*scall %s(&mem[%ld])\n", depth * 2, "", p->f[s->k].name, ref);
            long r = call_fn(p, (int)s->k, ref, depth + 1);
            fprintf(stderr, "  %*sreturn %ld\n", depth * 2, "", r);
            *a = r;
            break;
        }
        case OP_RET: {
            long r = *a;
            sp = base;
            return r;
        }
        case OP_PRINT_INT:
            printf("%ld\n", mem[base + s->b]);
            break;
        default: /* OP_PRINT_STR */
            printf("%s\n", s->s);
            break;
        }
    }
}

static void stage_run(const struct program *p)
{
    fprintf(stderr, "== stage_run ==\n");
    sp = 0;
    steps = 0;
    call_fn(p, 0, 0, 0);
    fprintf(stderr, "  %ld statements executed\n", steps);
}

/* Check every function. An accepted program runs. */
static void compile(const struct program *p)
{
    int errors = 0;
    for (int fi = 0; fi < p->nfuncs; fi++) {
        stage_facts(&p->f[fi]);
        stage_liveness();
        errors += stage_loans();
    }
    if (errors == 0) {
        fprintf(stderr, "borrow check passed\n");
        stage_run(p);
    } else {
        fprintf(stderr, "borrow check failed with %d error(s), the program is not run\n", errors);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2 || stage_source(argv[1]) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    compile(&prog);
    build_rejected();
    fprintf(stderr, "\n== a program the borrow checker rejects ==\n");
    print_program(&rejected);
    compile(&rejected);
    return 0;
}
