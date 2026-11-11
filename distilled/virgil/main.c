/*
 * virgil: a cut-down version of the whole-program middle end of Aeneas, the compiler of Virgil.
 *
 * Aeneas compiles a program as a whole. It first runs the program's initializers on an
 * interpreter and keeps the heap they build. Then a reachability analysis starts from `main` and
 * finds what the program can really use: the methods that can be called (a virtual method only if
 * a class that overrides it is live), the classes that can be allocated, the fields that are
 * read, and the objects of the initialized heap that live code can reach. Fields that are never
 * read are deleted, and a field that always holds the same value is replaced by that value.
 * Generic methods are not copied per type argument. The specializer puts the instantiations of a
 * method into groups by the polymorphic operations they perform, and by the groups of the
 * methods they call, so `id<A>` and `id<B>` share one copy when both are references and no
 * operation tells them apart. Finally normalization flattens tuples and objects into scalars.
 *
 * There is no parser or type checker: the example programs are built with the constructors of a
 * small typed tree IR. The language has int, classes (single inheritance, virtual methods, no
 * field inheritance), pairs, and generic functions with one type parameter T. A type argument is
 * a concrete type or T itself. There are no variants, closures, arrays or SSA graphs.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the result of `main` on stdout and the intermediate form after each stage on stderr.
 *   No machine code is made, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source     the program as written, with the initializer that builds the heap
 *   stage_init       the initializer runs on the interpreter and leaves a heap and globals
 *   stage_reach      reachability analysis: live classes, methods, fields and heap objects
 *   stage_specialize the instantiations of every generic function, grouped into shared copies
 *   stage_normalize  the memory layout of every live class and the signature of every copy,
 *                    in scalars; the heap is rewritten into that layout
 *   stage_run        main runs on the normalized heap and layouts
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TYPES 300
#define MAX_EXPRS 600
#define MAX_FNS 14
#define MAX_CLS 6
#define MAX_FIELDS 3
#define MAX_HEAP 16
#define MAX_SLOTS 8
#define MAX_INST 32
#define MAX_SIG 40
#define MAX_EDGES 8
#define MAX_VUSES 8
#define STACK 256

/* ---- types ---- */

/* int and class types take one slot, a pair takes the slots of both parts, and T takes the slots
 * of the type argument. */
enum tk { T_INT, T_CLASS, T_PAIR, T_VAR };

struct type {
    enum tk k;
    int cls;
    const struct type *a, *b;
};

static struct type types[MAX_TYPES];
static int ntypes;
static const struct type *t_int, *t_var;

static const struct type *mk_type(enum tk k, int cls, const struct type *a, const struct type *b)
{
    types[ntypes] = (struct type){k, cls, a, b};
    return &types[ntypes++];
}

static int size(const struct type *t, const struct type *targ)
{
    switch (t->k) {
    case T_PAIR: return size(t->a, targ) + size(t->b, targ);
    case T_VAR: return size(targ, NULL);
    default: return 1;
    }
}

static bool open_type(const struct type *t)
{
    switch (t->k) {
    case T_VAR: return true;
    case T_PAIR: return (int)open_type(t->a) | (int)open_type(t->b);
    default: return false;
    }
}

static bool type_eq(const struct type *x, const struct type *y)
{
    if (x->k != y->k || x->cls != y->cls) {
        return false;
    }
    if (x->k != T_PAIR) {
        return true;
    }
    return (int)type_eq(x->a, y->a) & (int)type_eq(x->b, y->b);
}

/* The scalar kinds of a type, one character per slot: i for an int, r for a reference. */
static int scalars(const struct type *t, const struct type *targ, char *out)
{
    switch (t->k) {
    case T_INT:
        out[0] = 'i';
        return 1;
    case T_CLASS:
        out[0] = 'r';
        return 1;
    case T_PAIR: {
        int n = scalars(t->a, targ, out);
        return n + scalars(t->b, targ, out + n);
    }
    default:
        return scalars(targ, NULL, out);
    }
}

/* ---- the program ---- */

struct cls {
    const char *name;
    int parent;
    int nf;
    const char *fname[MAX_FIELDS];
    const struct type *ft[MAX_FIELDS];
};

enum ek {
    E_INT, E_VAR, E_ADD, E_SUB, E_MUL, E_IF, E_NEW, E_NEWT, E_GETF, E_SETF, E_VCALL, E_CALL,
    E_TUPLE, E_PROJ, E_GGET, E_GSET, E_SEQ
};

struct expr {
    enum ek k;
    int n;                   /* constant, parameter, field, class, function, projection or global */
    const char *name;        /* method name of a virtual call */
    int nx;
    const struct expr *x[3]; /* operands, or arguments (the receiver first for a virtual call) */
    const struct type *t;    /* the type of the result, which may mention T */
    const struct type *targ; /* the type argument of a call */
};

struct fn {
    const char *name;
    int cls;                 /* the class of a method, or -1 */
    bool generic;
    int np;
    const struct type *pt[3];
    const struct type *ret;
    const struct expr *body;
};

struct global {
    const char *name;
    const struct type *t;
};

static struct cls cls[MAX_CLS];
static int ncls;
static struct fn fns[MAX_FNS];
static int nfns;
static struct global globals[2];
static int nglobals;
static struct expr exprs[MAX_EXPRS];
static int nexprs;
static int main_fn = -1, init_fn = -1;

static const struct type *class_type(int c)
{
    return mk_type(T_CLASS, c, NULL, NULL);
}

static int add_class(const char *name, int parent)
{
    cls[ncls].name = name;
    cls[ncls].parent = parent;
    return ncls++;
}

static void add_field(int c, const char *name, const struct type *t)
{
    cls[c].fname[cls[c].nf] = name;
    cls[c].ft[cls[c].nf++] = t;
}

static int add_global(const char *name, const struct type *t)
{
    globals[nglobals] = (struct global){name, t};
    return nglobals++;
}

static int add_fn(const char *name, int c, bool generic, const struct type *ret, int np, const struct type *p0,
                  const struct type *p1)
{
    fns[nfns] = (struct fn){.name = name, .cls = c, .generic = generic, .np = np, .ret = ret};
    fns[nfns].pt[0] = p0;
    fns[nfns].pt[1] = p1;
    return nfns++;
}

static void set_body(int f, const struct expr *body)
{
    fns[f].body = body;
}

/* The return type of a call with its T replaced. */
static const struct type *subst(const struct type *t, const struct type *targ)
{
    return t->k == T_VAR ? targ : t;
}

static struct expr *mk(enum ek k, int n, const struct type *t, int nx, const struct expr *a, const struct expr *b,
                       const struct expr *c)
{
    exprs[nexprs] = (struct expr){.k = k, .n = n, .nx = nx, .t = t, .x = {a, b, c}};
    return &exprs[nexprs++];
}

static const struct expr *x_int(int n)
{
    return mk(E_INT, n, t_int, 0, NULL, NULL, NULL);
}

static const struct expr *x_null(int c)
{
    return mk(E_INT, 0, class_type(c), 0, NULL, NULL, NULL);
}

static const struct expr *x_var(int i, const struct type *t)
{
    return mk(E_VAR, i, t, 0, NULL, NULL, NULL);
}

static const struct expr *x_bin(enum ek k, const struct expr *a, const struct expr *b)
{
    return mk(k, 0, t_int, 2, a, b, NULL);
}

static const struct expr *x_if(const struct expr *c, const struct expr *a, const struct expr *b)
{
    return mk(E_IF, 0, a->t, 3, c, a, b);
}

static const struct expr *x_new(int c, int nargs, const struct expr *a, const struct expr *b, const struct expr *d)
{
    return mk(E_NEW, c, class_type(c), nargs, a, b, d);
}

static const struct expr *x_newt(void)
{
    return mk(E_NEWT, 0, t_var, 0, NULL, NULL, NULL);
}

static const struct expr *x_getf(const struct expr *o, int i)
{
    return mk(E_GETF, i, cls[o->t->cls].ft[i], 1, o, NULL, NULL);
}

static const struct expr *x_setf(const struct expr *o, int i, const struct expr *v)
{
    return mk(E_SETF, i, v->t, 2, o, v, NULL);
}

static int find_method(int c, const char *name)
{
    for (;; c = cls[c].parent) {
        for (int f = 0; f < nfns; f++) {
            if ((fns[f].cls == c) & (strcmp(fns[f].name, name) == 0)) {
                return f;
            }
        }
    }
}

static const struct expr *x_vcall(const char *name, int nargs, const struct expr *o, const struct expr *a,
                                  const struct expr *b)
{
    struct expr *e = mk(E_VCALL, 0, fns[find_method(o->t->cls, name)].ret, nargs + 1, o, a, b);
    e->name = name;
    return e;
}

static const struct expr *x_call(int f, const struct type *targ, int nargs, const struct expr *a, const struct expr *b)
{
    const struct type *t = fns[f].ret;
    if (targ != NULL) {
        t = subst(t, targ);
    }
    struct expr *e = mk(E_CALL, f, t, nargs, a, b, NULL);
    e->targ = targ;
    return e;
}

static const struct expr *x_tuple(const struct expr *a, const struct expr *b)
{
    return mk(E_TUPLE, 0, mk_type(T_PAIR, 0, a->t, b->t), 2, a, b, NULL);
}

static const struct expr *x_proj(const struct expr *e, int i)
{
    return mk(E_PROJ, i, i == 0 ? e->t->a : e->t->b, 1, e, NULL, NULL);
}

static const struct expr *x_gget(int g)
{
    return mk(E_GGET, g, globals[g].t, 0, NULL, NULL, NULL);
}

static const struct expr *x_gset(int g, const struct expr *v)
{
    return mk(E_GSET, g, v->t, 1, v, NULL, NULL);
}

static const struct expr *x_seq(const struct expr *a, const struct expr *b)
{
    return mk(E_SEQ, 0, b->t, 2, a, b, NULL);
}

/* ---- printing ---- */

static void pr_type(const struct type *t)
{
    switch (t->k) {
    case T_INT: fprintf(stderr, "int"); break;
    case T_CLASS: fprintf(stderr, "%s", cls[t->cls].name); break;
    case T_PAIR:
        fprintf(stderr, "(");
        pr_type(t->a);
        fprintf(stderr, ",");
        pr_type(t->b);
        fprintf(stderr, ")");
        break;
    default: fprintf(stderr, "T"); break;
    }
}

static void pr_fn_name(int f, const struct type *targ)
{
    if (fns[f].cls >= 0) {
        fprintf(stderr, "%s.", cls[fns[f].cls].name);
    }
    fprintf(stderr, "%s", fns[f].name);
    if (targ != NULL) {
        fprintf(stderr, "<");
        pr_type(targ);
        fprintf(stderr, ">");
    }
}

static void pr_expr(const struct expr *e)
{
    static const char *const ops[] = {[E_ADD] = "add", [E_SUB] = "sub", [E_MUL] = "mul", [E_IF] = "if",
                                      [E_TUPLE] = "tuple", [E_SEQ] = "seq"};
    switch (e->k) {
    case E_INT: fprintf(stderr, e->t->k == T_CLASS ? "null" : "%d", e->n); break;
    case E_VAR: fprintf(stderr, "$%d", e->n); break;
    case E_NEW:
        fprintf(stderr, "(new %s", cls[e->n].name);
        break;
    case E_NEWT: fprintf(stderr, "(new T"); break;
    case E_GETF: fprintf(stderr, "(."); break;
    case E_SETF: fprintf(stderr, "(set"); break;
    case E_VCALL: fprintf(stderr, "(call-virtual %s", e->name); break;
    case E_CALL:
        fprintf(stderr, "(call ");
        pr_fn_name(e->n, e->targ);
        break;
    case E_PROJ: fprintf(stderr, "(proj %d", e->n); break;
    case E_GGET: fprintf(stderr, "@%s", globals[e->n].name); break;
    case E_GSET: fprintf(stderr, "(set @%s", globals[e->n].name); break;
    default: fprintf(stderr, "(%s", ops[e->k]); break;
    }
    for (int i = 0; i < e->nx; i++) {
        fprintf(stderr, " ");
        pr_expr(e->x[i]);
    }
    if (e->k == E_GETF) {
        fprintf(stderr, " %s", cls[e->x[0]->t->cls].fname[e->n]);
    }
    if (e->k != E_INT && e->k != E_VAR && e->k != E_GGET) {
        fprintf(stderr, ")");
    }
}

static void pr_sig(int f)
{
    fprintf(stderr, "(");
    for (int i = 0; i < fns[f].np; i++) {
        fprintf(stderr, "%s$%d: ", i ? ", " : "", i);
        pr_type(fns[f].pt[i]);
    }
    fprintf(stderr, ") -> ");
    pr_type(fns[f].ret);
}

static void stage_source(void)
{
    fprintf(stderr, "== stage_source ==\n");
    for (int c = 0; c < ncls; c++) {
        fprintf(stderr, "class %s", cls[c].name);
        if (cls[c].parent >= 0) {
            fprintf(stderr, " extends %s", cls[cls[c].parent].name);
        }
        fprintf(stderr, " {");
        for (int i = 0; i < cls[c].nf; i++) {
            fprintf(stderr, "%s%s: ", i ? ", " : " ", cls[c].fname[i]);
            pr_type(cls[c].ft[i]);
        }
        fprintf(stderr, " }\n");
    }
    for (int g = 0; g < nglobals; g++) {
        fprintf(stderr, "global %s: ", globals[g].name);
        pr_type(globals[g].t);
        fprintf(stderr, "\n");
    }
    for (int f = 0; f < nfns; f++) {
        fprintf(stderr, "fn ");
        pr_fn_name(f, NULL);
        fprintf(stderr, "%s", fns[f].generic ? "<T>" : "");
        pr_sig(f);
        fprintf(stderr, " = ");
        pr_expr(fns[f].body);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
}

/* ---- the interpreter, used at compile time by stage_init and again by stage_run ---- */

/* Where each field of each class lives in an object, in slots. A field with offset -1 has no
 * slot: it was deleted, or it holds `cst`. */
struct layout {
    int off[MAX_CLS][MAX_FIELDS];
    int nslot[MAX_CLS];
    int64_t cst[MAX_CLS][MAX_FIELDS];
};

struct obj {
    int cls; /* -1 for an object that does not exist in the layout */
    int64_t s[MAX_SLOTS];
};

static struct layout lay_src, lay_norm;
static const struct layout *lay;
static struct obj heap[MAX_HEAP]; /* a reference is an index, and 0 is null */
static int nheap = 1;
static int64_t gval[2][2];
static int64_t stk[STACK];
static int sp;
static bool tracing;

struct ctx {
    const struct fn *f;
    int base;
    const struct type *targ;
};

static void eval(const struct expr *e, const struct ctx *c);
static int copy_of(int f, const struct type *targ);

static int param_off(const struct fn *f, int i, const struct type *targ)
{
    int off = 0;
    for (int k = 0; k < i; k++) {
        off += size(f->pt[k], targ);
    }
    return off;
}

static void call(int fi, const struct type *targ, int base)
{
    const struct fn *f = &fns[fi];
    if (tracing && f->generic) {
        fprintf(stderr, "  call ");
        pr_fn_name(fi, targ);
        fprintf(stderr, " -> copy %d\n", copy_of(fi, targ));
    }
    struct ctx nc = {f, base, targ};
    eval(f->body, &nc);
    int rs = size(f->ret, targ);
    memmove(&stk[base], &stk[sp - rs], (size_t)rs * sizeof stk[0]);
    sp = base + rs;
}

static int new_object(int c)
{
    heap[nheap].cls = c;
    return nheap++;
}

static void eval(const struct expr *e, const struct ctx *c)
{
    int start = sp;
    switch (e->k) {
    case E_INT:
        stk[sp++] = e->n;
        break;
    case E_VAR: {
        int n = size(e->t, c->targ);
        memcpy(&stk[sp], &stk[c->base + param_off(c->f, e->n, c->targ)], (size_t)n * sizeof stk[0]);
        sp += n;
        break;
    }
    case E_ADD:
    case E_SUB:
    case E_MUL:
        eval(e->x[0], c);
        eval(e->x[1], c);
        sp--;
        if (e->k == E_ADD) {
            stk[sp - 1] += stk[sp];
        } else if (e->k == E_SUB) {
            stk[sp - 1] -= stk[sp];
        } else {
            stk[sp - 1] *= stk[sp];
        }
        break;
    case E_IF:
        eval(e->x[0], c);
        eval(stk[--sp] != 0 ? e->x[1] : e->x[2], c);
        break;
    case E_NEW: {
        for (int i = 0; i < e->nx; i++) {
            eval(e->x[i], c);
        }
        int o = new_object(e->n);
        int at = start;
        for (int i = 0; i < cls[e->n].nf; i++) {
            int n = size(cls[e->n].ft[i], NULL);
            if (lay->off[e->n][i] >= 0) {
                memcpy(&heap[o].s[lay->off[e->n][i]], &stk[at], (size_t)n * sizeof stk[0]);
            }
            at += n;
        }
        sp = start;
        stk[sp++] = o;
        break;
    }
    case E_NEWT:
        stk[sp++] = new_object(c->targ->cls);
        break;
    case E_GETF: {
        eval(e->x[0], c);
        int o = (int)stk[--sp], k = e->x[0]->t->cls, n = size(e->t, NULL);
        if (lay->off[k][e->n] >= 0) {
            memcpy(&stk[sp], &heap[o].s[lay->off[k][e->n]], (size_t)n * sizeof stk[0]);
        } else {
            stk[sp] = lay->cst[k][e->n];
        }
        sp += n;
        break;
    }
    case E_SETF: {
        eval(e->x[0], c);
        eval(e->x[1], c);
        int k = e->x[0]->t->cls, n = size(e->t, NULL), o = (int)stk[start];
        if (lay->off[k][e->n] >= 0) {
            memcpy(&heap[o].s[lay->off[k][e->n]], &stk[start + 1], (size_t)n * sizeof stk[0]);
        }
        memmove(&stk[start], &stk[start + 1], (size_t)n * sizeof stk[0]);
        sp--;
        break;
    }
    case E_VCALL:
    case E_CALL: {
        for (int i = 0; i < e->nx; i++) {
            eval(e->x[i], c);
        }
        if (e->k == E_VCALL) {
            call(find_method(heap[stk[start]].cls, e->name), NULL, start);
        } else {
            call(e->n, e->targ != NULL && e->targ->k == T_VAR ? c->targ : e->targ, start);
        }
        break;
    }
    case E_TUPLE:
        eval(e->x[0], c);
        eval(e->x[1], c);
        break;
    case E_PROJ: {
        const struct type *pair = e->x[0]->t;
        eval(e->x[0], c);
        int na = size(pair->a, c->targ), nb = size(pair->b, c->targ);
        if (e->n == 0) {
            sp -= nb;
        } else {
            memmove(&stk[start], &stk[start + na], (size_t)nb * sizeof stk[0]);
            sp -= na;
        }
        break;
    }
    case E_GGET: {
        int n = size(e->t, NULL);
        memcpy(&stk[sp], gval[e->n], (size_t)n * sizeof stk[0]);
        sp += n;
        break;
    }
    case E_GSET:
        eval(e->x[0], c);
        memcpy(gval[e->n], &stk[start], (size_t)size(e->t, NULL) * sizeof stk[0]);
        break;
    default: /* E_SEQ */
        eval(e->x[0], c);
        sp -= size(e->x[0]->t, c->targ);
        eval(e->x[1], c);
        break;
    }
}

/* ---- stage_init ---- */

static void make_source_layout(void)
{
    for (int k = 0; k < ncls; k++) {
        int at = 0;
        for (int i = 0; i < cls[k].nf; i++) {
            lay_src.off[k][i] = at;
            at += size(cls[k].ft[i], NULL);
        }
        lay_src.nslot[k] = at;
    }
}

static void pr_value(const struct type *t, const int64_t *v)
{
    switch (t->k) {
    case T_INT: fprintf(stderr, "%lld", (long long)v[0]); break;
    case T_CLASS:
        if (v[0] == 0) {
            fprintf(stderr, "null");
        } else {
            fprintf(stderr, "#%lld", (long long)v[0]);
        }
        break;
    default:
        fprintf(stderr, "(");
        pr_value(t->a, v);
        fprintf(stderr, ",");
        pr_value(t->b, v + size(t->a, NULL));
        fprintf(stderr, ")");
        break;
    }
}

static void pr_object(int o, const struct layout *l)
{
    int k = heap[o].cls;
    fprintf(stderr, "  #%d %s {", o, cls[k].name);
    int shown = 0;
    for (int i = 0; i < cls[k].nf; i++) {
        if (l->off[k][i] >= 0) {
            fprintf(stderr, "%s%s=", shown++ ? ", " : " ", cls[k].fname[i]);
            pr_value(cls[k].ft[i], &heap[o].s[l->off[k][i]]);
        }
    }
    fprintf(stderr, " }\n");
}

static void stage_init(void)
{
    fprintf(stderr, "== stage_init ==\n");
    make_source_layout();
    lay = &lay_src;
    call(init_fn, NULL, 0);
    for (int o = 1; o < nheap; o++) {
        pr_object(o, &lay_src);
    }
    for (int g = 0; g < nglobals; g++) {
        fprintf(stderr, "  @%s = ", globals[g].name);
        pr_value(globals[g].t, gval[g]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
}

/* ---- stage_reach ---- */

/* What the analysis knows about a field: whether it is read or written, and the values it holds. `count` is 0 for none, 1 for a single known value and 2 for many. */
struct fact {
    bool read, written;
    int count;
    int64_t val;
};

static struct fact facts[MAX_CLS][MAX_FIELDS];
static bool cls_live[MAX_CLS];
static bool obj_live[MAX_HEAP];
static bool glob_read[2];

/* A function, or one instantiation of a generic function. Its `sig` lists the polymorphic
 * operations in its body and `calls` the instances it calls, in order. */
struct inst {
    int fn;
    const struct type *targ;
    int calls[MAX_EDGES];
    int ncalls;
    int sig[MAX_SIG];
    int nsig;
    int group;
    int copy;
};

static struct inst insts[MAX_INST];
static int ninst;

struct vuse {
    int cls;
    const char *name;
};

static struct vuse vuses[MAX_VUSES];
static int nvuses;

static void add_value(struct fact *f, bool known, int64_t v)
{
    if (!known) {
        f->count = 2;
    } else if (f->count == 0) {
        f->count = 1;
        f->val = v;
    } else if (f->val != v) {
        f->count = 2;
    }
}

static int get_inst(int fn, const struct type *targ)
{
    for (int i = 0; i < ninst; i++) {
        if (insts[i].fn == fn && (targ == NULL || type_eq(insts[i].targ, targ))) {
            return i;
        }
    }
    insts[ninst] = (struct inst){.fn = fn, .targ = targ};
    return ninst++;
}

static bool is_sub(int c, int parent)
{
    while (c != parent && c >= 0) {
        c = cls[c].parent;
    }
    return c == parent;
}

static void class_live(int c)
{
    if (cls_live[c]) {
        return;
    }
    cls_live[c] = true;
    for (int v = 0; v < nvuses; v++) {
        if (is_sub(c, vuses[v].cls)) {
            get_inst(find_method(c, vuses[v].name), NULL);
        }
    }
}

static void reach_object(int o);

/* A live object holds a value in a field that live code reads. Globals hold references. */
static void read_value(int o, int i)
{
    int k = heap[o].cls;
    struct fact *f = &facts[k][i];
    if (size(cls[k].ft[i], NULL) != 1) {
        add_value(f, false, 0);
        return;
    }
    int64_t v = heap[o].s[lay_src.off[k][i]];
    add_value(f, true, v);
    if (cls[k].ft[i]->k == T_CLASS) {
        reach_object((int)v);
    }
}

static void reach_object(int o)
{
    if (o == 0 || obj_live[o]) {
        return;
    }
    obj_live[o] = true;
    class_live(heap[o].cls);
    for (int i = 0; i < cls[heap[o].cls].nf; i++) {
        if (facts[heap[o].cls][i].read) {
            read_value(o, i);
        }
    }
}

static void field_read(int k, int i)
{
    if (facts[k][i].read) {
        return;
    }
    facts[k][i].read = true;
    for (int o = 1; o < nheap; o++) {
        if (obj_live[o] && heap[o].cls == k) {
            read_value(o, i);
        }
    }
}

static void sig_put(struct inst *in, int v)
{
    in->sig[in->nsig++] = v;
}

/* The scalar kinds of a callee's parameters and result that mention T, for the signature. */
static void sig_callconv(struct inst *in, int callee, const struct type *targ)
{
    const struct fn *g = &fns[callee];
    char kinds[8];
    for (int i = 0; i <= g->np; i++) {
        const struct type *t = i < g->np ? g->pt[i] : g->ret;
        int n = scalars(t, targ, kinds);
        for (int k = 0; k < n; k++) {
            sig_put(in, kinds[k]);
        }
        sig_put(in, '|');
    }
}

static void walk(const struct expr *e, int ii)
{
    for (int i = 0; i < e->nx; i++) {
        walk(e->x[i], ii);
    }
    const struct type *targ = insts[ii].targ;
    switch (e->k) {
    case E_NEW:
        class_live(e->n);
        for (int i = 0; i < cls[e->n].nf; i++) {
            struct fact *f = &facts[e->n][i];
            add_value(f, e->x[i]->k == E_INT, e->x[i]->n);
        }
        break;
    case E_NEWT:
        /* `new T`: the class is part of the instantiation, so it goes into the signature */
        class_live(targ->cls);
        sig_put(&insts[ii], targ->cls);
        for (int i = 0; i < cls[targ->cls].nf; i++) {
            add_value(&facts[targ->cls][i], true, 0);
        }
        break;
    case E_GETF:
        field_read(e->x[0]->t->cls, e->n);
        break;
    case E_SETF: {
        struct fact *f = &facts[e->x[0]->t->cls][e->n];
        f->written = true;
        f->count = 2; /* a written field is never constant */
        break;
    }
    case E_VCALL: {
        int sc = e->x[0]->t->cls;
        vuses[nvuses++] = (struct vuse){sc, e->name};
        for (int c = 0; c < ncls; c++) {
            if (cls_live[c] && is_sub(c, sc)) {
                get_inst(find_method(c, e->name), NULL);
            }
        }
        break;
    }
    case E_CALL: {
        const struct type *ct = e->targ != NULL && e->targ->k == T_VAR ? targ : e->targ;
        int callee = get_inst(e->n, ct);
        insts[ii].calls[insts[ii].ncalls++] = callee;
        if (e->targ != NULL && e->targ->k == T_VAR) {
            sig_callconv(&insts[ii], e->n, ct);
        }
        break;
    }
    case E_TUPLE:
        if (open_type(e->t)) {
            sig_put(&insts[ii], 't' * 100 + size(e->t, targ));
        }
        break;
    case E_PROJ:
        if (open_type(e->x[0]->t)) {
            const struct type *pair = e->x[0]->t;
            int na = size(pair->a, targ);
            sig_put(&insts[ii], 'p' * 100 + (e->n == 0 ? 0 : na));
            sig_put(&insts[ii], size(e->n == 0 ? pair->a : pair->b, targ));
        }
        break;
    case E_GGET:
        if (!glob_read[e->n]) {
            glob_read[e->n] = true;
            reach_object((int)gval[e->n][0]);
        }
        break;
    default:
        break;
    }
}

static void pr_inst_name(int i)
{
    pr_fn_name(insts[i].fn, insts[i].targ);
}

static void stage_reach(void)
{
    fprintf(stderr, "== stage_reach ==\n");
    get_inst(main_fn, NULL);
    for (int i = 0; i < ninst; i++) {
        if (fns[insts[i].fn].generic) {
            sig_callconv(&insts[i], insts[i].fn, insts[i].targ);
        }
        walk(fns[insts[i].fn].body, i);
    }
    fprintf(stderr, "live functions:\n");
    for (int i = 0; i < ninst; i++) {
        fprintf(stderr, "  ");
        pr_inst_name(i);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "live classes:");
    for (int k = 0; k < ncls; k++) {
        if (cls_live[k]) {
            fprintf(stderr, " %s", cls[k].name);
        }
    }
    fprintf(stderr, "\nfields:\n");
    for (int k = 0; k < ncls; k++) {
        for (int i = 0; cls_live[k] && i < cls[k].nf; i++) {
            const struct fact *f = &facts[k][i];
            fprintf(stderr, "  %s.%s:%s%s", cls[k].name, cls[k].fname[i], f->read ? " read" : " never-read",
                    f->written ? " written" : "");
            if (f->read) {
                fprintf(stderr, f->count == 1 ? " value=%lld" : " values=many", (long long)f->val);
            }
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "heap:");
    for (int o = 1; o < nheap; o++) {
        fprintf(stderr, " #%d=%s", o, obj_live[o] ? "live" : "dead");
    }
    fprintf(stderr, "\n\n");
}

/* ---- stage_specialize ---- */

static int ngroups;

static bool same_key(const int *x, const int *y, int n)
{
    return memcmp(x, y, (size_t)n * sizeof x[0]) == 0;
}

/* The instantiations of one function are one group when their signatures match, and the
 * groups they call match too. Starting from the signatures, a group is split whenever two of its
 * instances call different groups, until nothing changes. */
static void stage_specialize(void)
{
    fprintf(stderr, "== stage_specialize ==\n");
    int gid[MAX_INST];
    for (int i = 0; i < ninst; i++) {
        gid[i] = i;
        for (int j = 0; j < i; j++) {
            if (insts[j].fn == insts[i].fn && insts[j].nsig == insts[i].nsig &&
                same_key(insts[j].sig, insts[i].sig, insts[i].nsig)) {
                gid[i] = gid[j];
                break;
            }
        }
    }
    int old = -1;
    for (int round = 0;; round++) {
        int n = 0;
        for (int i = 0; i < ninst; i++) {
            n += gid[i] == i;
        }
        fprintf(stderr, "round %d: %d groups\n", round, n);
        if (n == old) {
            break;
        }
        old = n;
        int key[MAX_INST][MAX_EDGES + 1], next[MAX_INST];
        for (int i = 0; i < ninst; i++) {
            key[i][0] = gid[i];
            for (int k = 0; k < insts[i].ncalls; k++) {
                key[i][k + 1] = gid[insts[i].calls[k]];
            }
            next[i] = i;
            for (int j = 0; j < i; j++) {
                if (insts[j].fn == insts[i].fn && same_key(key[j], key[i], insts[i].ncalls + 1)) {
                    next[i] = next[j];
                    break;
                }
            }
        }
        memcpy(gid, next, sizeof gid);
    }
    for (int f = 0; f < nfns; f++) {
        int copies = 0;
        for (int i = 0; i < ninst; i++) {
            if (insts[i].fn == f) {
                insts[i].group = gid[i];
                insts[i].copy = gid[i] == i ? copies++ : insts[gid[i]].copy;
            }
        }
        for (int k = 0; k < copies && fns[f].generic; k++) {
            fprintf(stderr, "%s copy %d:", fns[f].name, k);
            for (int i = 0; i < ninst; i++) {
                if (insts[i].fn == f && insts[i].copy == k) {
                    fprintf(stderr, " <");
                    pr_type(insts[i].targ);
                    fprintf(stderr, ">");
                }
            }
            fprintf(stderr, "\n");
        }
    }
    ngroups = old;
    fprintf(stderr, "\n");
}

static int copy_of(int f, const struct type *targ)
{
    return insts[get_inst(f, targ)].copy;
}

/* ---- stage_normalize ---- */

static bool is_const(const struct fact *f)
{
    return f->count == 1;
}

static void stage_normalize(void)
{
    fprintf(stderr, "== stage_normalize ==\n");
    for (int k = 0; k < ncls; k++) {
        if (!cls_live[k]) {
            continue;
        }
        int at = 0;
        fprintf(stderr, "class %s:", cls[k].name);
        for (int i = 0; i < cls[k].nf; i++) {
            const struct fact *f = &facts[k][i];
            lay_norm.off[k][i] = -1;
            fprintf(stderr, " %s=", cls[k].fname[i]);
            if (!f->read) {
                fprintf(stderr, "deleted");
            } else if (is_const(f)) {
                lay_norm.cst[k][i] = f->val;
                fprintf(stderr, "const(%lld)", (long long)f->val);
            } else {
                lay_norm.off[k][i] = at;
                fprintf(stderr, "slot %d", at);
                at += size(cls[k].ft[i], NULL);
            }
        }
        lay_norm.nslot[k] = at;
        fprintf(stderr, "  (%d slots, was %d)\n", at, lay_src.nslot[k]);
    }
    for (int i = 0; i < ninst; i++) {
        if (insts[i].group != i) {
            continue;
        }
        char kinds[8];
        const struct fn *f = &fns[insts[i].fn];
        fprintf(stderr, "copy ");
        pr_fn_name(insts[i].fn, NULL);
        fprintf(stderr, "#%d (", insts[i].copy);
        for (int p = 0; p < f->np; p++) {
            int n = scalars(f->pt[p], insts[i].targ, kinds);
            fprintf(stderr, "%s%.*s", p ? ", " : "", n, kinds);
        }
        int n = scalars(f->ret, insts[i].targ, kinds);
        fprintf(stderr, ") -> %.*s\n", n, kinds);
    }
    /* the heap is rewritten into the new layout; objects that live code cannot reach are dropped */
    for (int o = 1; o < nheap; o++) {
        struct obj old = heap[o];
        int k = old.cls;
        memset(heap[o].s, 0, sizeof heap[o].s);
        heap[o].cls = -1;
        if (!obj_live[o]) {
            continue;
        }
        heap[o].cls = k;
        for (int i = 0; i < cls[k].nf; i++) {
            if (lay_norm.off[k][i] >= 0) {
                memcpy(&heap[o].s[lay_norm.off[k][i]], &old.s[lay_src.off[k][i]],
                       (size_t)size(cls[k].ft[i], NULL) * sizeof old.s[0]);
            }
        }
        pr_object(o, &lay_norm);
    }
    fprintf(stderr, "%d copies of functions\n\n", ngroups);
}

/* ---- stage_run ---- */

static void stage_run(void)
{
    fprintf(stderr, "== stage_run ==\n");
    lay = &lay_norm;
    tracing = true;
    sp = 0;
    call(main_fn, NULL, 0);
    fprintf(stderr, "result %lld\n", (long long)stk[0]);
    printf("%lld\n", (long long)stk[0]);
}

/* ---- the example programs ---- */

struct example {
    const char *name;
    void (*build)(void);
};

static void finish(int init_f, int main_f, const struct expr *init_body, const struct expr *main_body)
{
    init_fn = init_f;
    main_fn = main_f;
    set_body(init_f, init_body);
    set_body(main_f, main_body);
}

static void build_shapes(void)
{
    int shape = add_class("Shape", -1), square = add_class("Square", shape), circle = add_class("Circle", shape);
    int tri = add_class("Tri", shape), node = add_class("Node", -1);
    add_field(square, "side", t_int);
    add_field(square, "tag", t_int);
    add_field(circle, "r", t_int);
    add_field(tri, "b", t_int);
    add_field(tri, "h", t_int);
    add_field(node, "shape", class_type(shape));
    add_field(node, "next", class_type(node));
    add_field(node, "extra", class_type(shape));
    int dot = add_class("Dot", shape);
    int head = add_global("head", class_type(node));
    int sq = add_global("sq", class_type(square));
    int a0 = add_fn("area", shape, false, t_int, 1, class_type(shape), NULL);
    int a1 = add_fn("area", square, false, t_int, 1, class_type(square), NULL);
    int a2 = add_fn("area", circle, false, t_int, 1, class_type(circle), NULL);
    int a3 = add_fn("area", tri, false, t_int, 1, class_type(tri), NULL);
    int sum = add_fn("sum", -1, false, t_int, 1, class_type(node), NULL);
    int mkc = add_fn("mkc", -1, false, class_type(shape), 0, NULL, NULL);
    int in = add_fn("init", -1, false, t_int, 0, NULL, NULL);
    int mn = add_fn("main", -1, false, t_int, 0, NULL, NULL);
    const struct type *ts = class_type(square), *tc = class_type(circle), *tt = class_type(tri), *tn = class_type(node);
    set_body(a0, x_int(0));
    set_body(a1, x_bin(E_MUL, x_getf(x_var(0, ts), 0), x_getf(x_var(0, ts), 0)));
    set_body(a2, x_bin(E_MUL, x_bin(E_MUL, x_getf(x_var(0, tc), 0), x_getf(x_var(0, tc), 0)), x_int(3)));
    set_body(a3, x_bin(E_MUL, x_getf(x_var(0, tt), 0), x_getf(x_var(0, tt), 1)));
    const struct expr *n = x_var(0, tn);
    set_body(sum, x_if(n, x_bin(E_ADD, x_vcall("area", 0, x_getf(n, 0), NULL, NULL), x_call(sum, NULL, 1, x_getf(n, 1), NULL)),
                       x_int(0)));
    set_body(mkc, x_new(circle, 1, x_int(5), NULL, NULL));
    const struct expr *n4 = x_new(node, 3, x_gget(sq), x_null(node), x_null(shape));
    const struct expr *n3 = x_new(node, 3, x_new(dot, 0, NULL, NULL, NULL), n4, x_null(shape));
    const struct expr *n2 = x_new(node, 3, x_new(square, 2, x_int(4), x_int(7), NULL), n3,
                                  x_new(tri, 2, x_int(1), x_int(2), NULL));
    const struct expr *n1 = x_new(node, 3, x_gget(sq), n2, x_new(tri, 2, x_int(3), x_int(4), NULL));
    const struct expr *init = x_seq(x_gset(sq, x_new(square, 2, x_int(3), x_int(7), NULL)), x_seq(x_gset(head, n1), x_int(0)));
    const struct expr *total = x_bin(E_ADD, x_call(sum, NULL, 1, x_gget(head), NULL),
                                     x_vcall("area", 0, x_call(mkc, NULL, 0, NULL, NULL), NULL, NULL));
    finish(in, mn, init, x_bin(E_ADD, total, x_vcall("area", 0, x_new(circle, 1, x_int(5), NULL, NULL), NULL, NULL)));
}

static const struct expr *rt_call(const struct type *targ, const struct expr *arg)
{
    return x_call(4, targ, 1, arg, NULL);
}

static void build_generics(void)
{
    int a = add_class("A", -1), b = add_class("B", -1);
    add_field(a, "v", t_int);
    add_field(b, "w", t_int);
    const struct type *pair_t = mk_type(T_PAIR, 0, t_var, t_var);
    int id = add_fn("id", -1, true, t_var, 1, t_var, NULL);                   /* 0 */
    int dup = add_fn("dup", -1, true, pair_t, 1, t_var, NULL);               /* 1 */
    int first = add_fn("first", -1, true, t_var, 1, t_var, NULL);            /* 2 */
    int last = add_fn("last", -1, true, t_var, 1, t_var, NULL);              /* 3 */
    int rt = add_fn("rt", -1, true, t_var, 1, t_var, NULL);                  /* 4 */
    int alloc = add_fn("alloc", -1, true, t_var, 0, NULL, NULL);             /* 5 */
    int fresh = add_fn("fresh", -1, true, t_var, 0, NULL, NULL);             /* 6 */
    int in = add_fn("init", -1, false, t_int, 0, NULL, NULL);
    int mn = add_fn("main", -1, false, t_int, 0, NULL, NULL);
    set_body(id, x_var(0, t_var));
    set_body(dup, x_tuple(x_var(0, t_var), x_var(0, t_var)));
    set_body(first, x_proj(x_call(dup, t_var, 1, x_var(0, t_var), NULL), 0));
    set_body(last, x_proj(x_call(dup, t_var, 1, x_var(0, t_var), NULL), 1));
    set_body(rt, x_call(first, t_var, 1, x_call(id, t_var, 1, x_var(0, t_var), NULL), NULL));
    set_body(alloc, x_newt());
    set_body(fresh, x_call(alloc, t_var, 0, NULL, NULL));
    const struct type *ta = class_type(a), *tb = class_type(b), *tp = mk_type(T_PAIR, 0, t_int, t_int);
    const struct expr *pr = rt_call(tp, x_tuple(x_int(2), x_int(3)));
    const struct expr *sum = x_bin(E_ADD, rt_call(t_int, x_int(3)),
                                   x_bin(E_ADD, x_getf(rt_call(ta, x_new(a, 1, x_bin(E_ADD, x_int(1), x_int(3)), NULL, NULL)), 0),
                                         x_getf(rt_call(tb, x_new(b, 1, x_int(5), NULL, NULL)), 0)));
    sum = x_bin(E_ADD, sum, x_bin(E_ADD, x_proj(pr, 0), x_proj(pr, 1)));
    sum = x_bin(E_ADD, sum, x_call(last, t_int, 1, x_int(7), NULL));
    sum = x_bin(E_ADD, sum, x_bin(E_ADD, x_getf(x_call(fresh, ta, 0, NULL, NULL), 0),
                                  x_getf(x_call(fresh, tb, 0, NULL, NULL), 0)));
    finish(in, mn, x_int(0), sum);
}

static void build_tuple_field(void)
{
    int seg = add_class("Seg", -1);
    const struct type *pt = mk_type(T_PAIR, 0, t_int, t_int);
    add_field(seg, "a", pt);
    add_field(seg, "b", pt);
    add_field(seg, "origin", pt);
    int s = add_global("s", class_type(seg));
    int dist = add_fn("dist", -1, false, t_int, 1, class_type(seg), NULL);
    int in = add_fn("init", -1, false, t_int, 0, NULL, NULL);
    int mn = add_fn("main", -1, false, t_int, 0, NULL, NULL);
    const struct expr *o = x_var(0, class_type(seg));
    const struct expr *dx = x_bin(E_SUB, x_proj(x_getf(o, 1), 0), x_proj(x_getf(o, 0), 0));
    const struct expr *dy = x_bin(E_SUB, x_proj(x_getf(o, 1), 1), x_proj(x_getf(o, 0), 1));
    set_body(dist, x_bin(E_ADD, x_bin(E_MUL, dx, dx), x_bin(E_MUL, dy, dy)));
    const struct expr *v = x_new(seg, 3, x_tuple(x_int(1), x_int(2)), x_tuple(x_int(4), x_int(6)),
                                 x_tuple(x_int(0), x_int(0)));
    finish(in, mn, x_seq(x_gset(s, v), x_int(0)), x_call(dist, NULL, 1, x_gget(s), NULL));
}

static void build_const_field(void)
{
    int cfg = add_class("Cfg", -1);
    add_field(cfg, "scale", t_int);
    add_field(cfg, "count", t_int);
    add_field(cfg, "log", t_int);
    int g = add_global("cfg", class_type(cfg));
    int bump = add_fn("bump", -1, false, t_int, 2, class_type(cfg), t_int);
    int in = add_fn("init", -1, false, t_int, 0, NULL, NULL);
    int mn = add_fn("main", -1, false, t_int, 0, NULL, NULL);
    const struct expr *c = x_var(0, class_type(cfg)), *by = x_var(1, t_int);
    set_body(bump, x_seq(x_setf(c, 2, by), x_setf(c, 1, x_bin(E_ADD, x_getf(c, 1), by))));
    const struct expr *body = x_bin(E_MUL, x_getf(x_gget(g), 0), x_getf(x_gget(g), 1));
    body = x_seq(x_call(bump, NULL, 2, x_gget(g), x_int(1)), x_seq(x_call(bump, NULL, 2, x_gget(g), x_int(1)), body));
    finish(in, mn, x_seq(x_gset(g, x_new(cfg, 3, x_int(7), x_int(0), x_int(0))), x_int(0)), body);
}

static const struct example examples[] = {
    {"const-field", build_const_field},
    {"generics", build_generics},
    {"shapes", build_shapes},
    {"tuple-field", build_tuple_field},
};

int main(int argc, char **argv)
{
    const struct example *ex = NULL;
    for (size_t i = 0; argc >= 2 && i < sizeof examples / sizeof examples[0]; i++) {
        if (strcmp(argv[1], examples[i].name) == 0) {
            ex = &examples[i];
        }
    }
    if (ex == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    t_int = mk_type(T_INT, 0, NULL, NULL);
    t_var = mk_type(T_VAR, 0, NULL, NULL);
    ex->build();
    stage_source();
    stage_init();
    stage_reach();
    stage_specialize();
    stage_normalize();
    stage_run();
    return 0;
}
