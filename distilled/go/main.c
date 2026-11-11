/*
 * go: a cut-down version of the escape analysis of the Go compiler (cmd/compile/internal/escape).
 *
 * Go decides at compile time whether a variable lives in a stack frame or on the heap. It
 * builds a graph whose vertices ("locations") are variables and every expression that
 * allocates (a box for an interface value, a slice literal, a closure), and whose edges are
 * assignments. The weight of an edge is the number of dereferences minus the number of
 * address-of operations: `p = &q` is -1, `p = q` is 0, `p = *q` is 1. Then it walks the graph
 * from every location, and when an address (a negative weight) reaches a location that
 * outlives the one whose address it is, that one is moved to the heap. A function's parameters
 * are summarized as tags ("leaking param content: a") that the callers of the function use
 * instead of looking at its body.
 *
 * Lua is dynamically typed, so each example is shown as the Go program that a Go implementation
 * of Lua would be: every Lua value is an `any`, and every number that is stored in one is a box.
 * That is what gives the analysis something to decide. The compiler's own front end (parsing,
 * type checking, inlining) is not here: a function per example builds the typed tree the escape
 * analysis receives.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. The program is run by an interpreter that puts each allocation where the analysis
 *   says, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source  show the Lua source of the example
 *   stage_build   show the typed tree of the Go program that stands for it
 *   stage_escape  per batch of functions, callees first: build the location graph, flow the
 *                 captured variables into the closures, walk the graph from every location,
 *                 and tag the parameters
 *   stage_run     run the program, with every allocation on the stack or the heap as decided
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_NODES 64
#define MAX_STMTS 48
#define MAX_BLOCKS 16
#define MAX_VARS 16
#define MAX_FUNCS 4
#define MAX_LOCS 32
#define MAX_EDGES 8
#define MAX_CLOSURES 2
#define STACK_CELLS 128
#define HEAP_CELLS 128
#define HEAP_BASE 1000000L
#define SINK_ADDR HEAP_BASE /* the global variable `sink` is the first cell of the heap */
#define UNREACHED 1000

/* ------------------------------------------------------------------------------------------
 * The typed tree
 * ---------------------------------------------------------------------------------------- */

enum node_kind {
    N_INT,     /* val */
    N_STR,     /* val: index in strings */
    N_NAME,    /* var */
    N_BIN,     /* op, x, y: an int operation */
    N_INDEX,   /* x[y] on a slice of any */
    N_LEN,     /* len(x) */
    N_UNBOX,   /* x.(int) */
    N_BOX,     /* any(x): val is 1 when x is a string. Allocates a box for the value */
    N_SLICE,   /* []any{x}. Allocates the array */
    N_CLOSURE, /* func literal, fn. Allocates the closure record */
    N_CALL     /* fn(y). x is the closure being called, if the callee is not a global function */
};

struct node {
    enum node_kind kind;
    long val;
    int var;
    char op;
    int fn;
    struct node *x, *y;
    bool ptr;       /* does the type of this expression contain pointers? */
    bool heap;      /* decided by the analysis, for nodes that allocate */
    char text[96]; /* how the node prints, set when the node gets a location */
};

enum stmt_kind { S_DECL, S_ASSIGN, S_IF, S_WHILE, S_RETURN, S_CALL, S_PRINT };

struct stmt {
    enum stmt_kind kind;
    int var;
    bool define; /* `v := e` instead of `v = e` */
    struct node *e;
    struct block *body, *els;
};

struct block {
    struct stmt *s[8];
    int n;
};

struct var {
    const char *name;
    const char *type;
    int func; /* the function that declares it, -1 for a global */
    bool ptr;
    bool global;
    bool heap; /* decided by the analysis */
};

struct hole {
    int dst;
    int derefs;
    bool addrtaken;
};

struct func {
    const char *name;
    const char *sig;
    int param;    /* the variable of its one parameter, or -1 */
    int result;   /* the variable of its one result, or -1 */
    int captured; /* a closure: the variable of the enclosing function that it uses, else -1 */
    bool closure;
    struct block *body;
    struct hole tag; /* where an argument for the parameter flows to */
    int batch;
};

enum { F_PRINTLN1, F_PRINTLN, F_MAIN, F_FACT };

static const char *const strings[] = {"Hello, world!"};

static struct node node_pool[MAX_NODES];
static struct stmt stmt_pool[MAX_STMTS];
static struct block blocks[MAX_BLOCKS];
static struct var vars[MAX_VARS];
static struct func funcs[MAX_FUNCS];
static int nnodes, nstmts, nblocks, nvars, nfuncs;

static int add_var(const char *name, const char *type, int func, bool ptr, bool global)
{
    vars[nvars] = (struct var){name, type, func, ptr, global, false};
    return nvars++;
}

static int add_func(const char *name, const char *sig, bool closure)
{
    funcs[nfuncs] = (struct func){name, sig, -1, -1, -1, closure, &blocks[nblocks++],
                                  {0, 0, false}, -1};
    return nfuncs++;
}

static struct node *mk(enum node_kind kind, bool ptr)
{
    struct node *n = &node_pool[nnodes++];
    n->kind = kind;
    n->ptr = ptr;
    n->fn = -1;
    return n;
}

static struct node *n_int(long val)
{
    struct node *n = mk(N_INT, false);
    n->val = val;
    return n;
}

static struct node *n_str(long index)
{
    struct node *n = mk(N_STR, false);
    n->val = index;
    return n;
}

static struct node *n_name(int var)
{
    struct node *n = mk(N_NAME, vars[var].ptr);
    n->var = var;
    return n;
}

static struct node *n_bin(char op, struct node *x, struct node *y)
{
    struct node *n = mk(N_BIN, false);
    n->op = op;
    n->x = x;
    n->y = y;
    return n;
}

static struct node *n_index(struct node *x, struct node *y)
{
    struct node *n = mk(N_INDEX, true);
    n->x = x;
    n->y = y;
    return n;
}

static struct node *n_len(struct node *x)
{
    struct node *n = mk(N_LEN, false);
    n->x = x;
    return n;
}

static struct node *n_unbox(struct node *x)
{
    struct node *n = mk(N_UNBOX, false);
    n->x = x;
    return n;
}

static struct node *n_box(struct node *x)
{
    struct node *n = mk(N_BOX, true);
    n->x = x;
    n->val = x->kind == N_STR;
    return n;
}

static struct node *n_slice(struct node *x)
{
    struct node *n = mk(N_SLICE, true);
    n->x = x;
    return n;
}

static struct node *n_closure(int fn)
{
    struct node *n = mk(N_CLOSURE, true);
    n->fn = fn;
    return n;
}

/* A call of a function with one argument that returns an any. */
static struct node *n_call(int fn, struct node *closure, struct node *arg)
{
    struct node *n = mk(N_CALL, true);
    n->fn = fn;
    n->x = closure;
    n->y = arg;
    return n;
}

static struct stmt *add_stmt(struct block *b, enum stmt_kind kind, int var, bool define,
                             struct node *e)
{
    struct stmt *s = &stmt_pool[nstmts++];
    *s = (struct stmt){kind, var, define, e, NULL, NULL};
    if (kind == S_IF || kind == S_WHILE) {
        s->body = &blocks[nblocks++];
    }
    if (kind == S_IF) {
        s->els = &blocks[nblocks++]; /* empty when there is no else */
    }
    b->s[b->n++] = s;
    return s;
}

/* `println(a ...any)` of the example: Println([]any{x}). */
static void println_of(struct block *b, struct node *x)
{
    add_stmt(b, S_CALL, 0, false, n_call(F_PRINTLN, NULL, n_slice(x)));
}

/* The two library functions that every example prints through. In the real standard library
 * these are fmt.Println and the writer it ends in. */
static void build_prelude(void)
{
    nnodes = nstmts = nblocks = nvars = nfuncs = 0;
    int sink = add_var("sink", "any", -1, true, true);

    /* func println1(x any) { print(x) }: the one place that writes. */
    int f1 = add_func("println1", "(x any)", false);
    int x = add_var("x", "any", f1, true, false);
    funcs[f1].param = x;
    add_stmt(funcs[f1].body, S_PRINT, 0, false, n_name(x));

    /* func Println(a []any): the arguments are handed on through an interface in the real
     * thing, which the compiler has to assume keeps them, so here `sink = a[0]` stands for that. */
    int f2 = add_func("Println", "(a []any)", false);
    int a = add_var("a", "[]any", f2, true, false);
    int i = add_var("i", "int", f2, false, false);
    funcs[f2].param = a;
    struct block *b = funcs[f2].body;
    add_stmt(b, S_ASSIGN, sink, false, n_index(n_name(a), n_int(0)));
    add_stmt(b, S_ASSIGN, i, true, n_int(0));
    struct stmt *loop = add_stmt(b, S_WHILE, 0, false, n_bin('<', n_name(i), n_len(n_name(a))));
    add_stmt(loop->body, S_CALL, 0, false, n_call(F_PRINTLN1, NULL, n_index(n_name(a), n_name(i))));
    add_stmt(loop->body, S_ASSIGN, i, false, n_bin('+', n_name(i), n_int(1)));
}

static struct block *begin_main(void)
{
    build_prelude();
    return funcs[add_func("main", "()", false)].body;
}

static void build_hello(void)
{
    struct block *b = begin_main();
    println_of(b, n_box(n_str(0)));
}

static void build_arithmetic(void)
{
    struct block *b = begin_main();
    struct node *sum = n_bin('+', n_int(2), n_int(3));
    println_of(b, n_box(n_bin('-', n_bin('*', sum, n_int(4)), n_int(6))));
}

static void build_branch(void)
{
    struct block *b = begin_main();
    int a = add_var("a", "any", F_MAIN, true, false);
    int c = add_var("b", "any", F_MAIN, true, false);
    add_stmt(b, S_ASSIGN, a, true, n_box(n_int(3)));
    add_stmt(b, S_ASSIGN, c, true, n_box(n_int(5)));
    struct stmt *branch =
        add_stmt(b, S_IF, 0, false, n_bin('<', n_unbox(n_name(a)), n_unbox(n_name(c))));
    println_of(branch->body, n_name(c));
    println_of(branch->els, n_name(a));
}

static void build_loop(void)
{
    struct block *b = begin_main();
    int sum = add_var("sum", "any", F_MAIN, true, false);
    int i = add_var("i", "any", F_MAIN, true, false);
    add_stmt(b, S_ASSIGN, sum, true, n_box(n_int(0)));
    add_stmt(b, S_ASSIGN, i, true, n_box(n_int(1)));
    struct stmt *loop =
        add_stmt(b, S_WHILE, 0, false, n_bin('<', n_unbox(n_name(i)), n_int(11)));
    add_stmt(loop->body, S_ASSIGN, sum, false,
             n_box(n_bin('+', n_unbox(n_name(sum)), n_unbox(n_name(i)))));
    add_stmt(loop->body, S_ASSIGN, i, false, n_box(n_bin('+', n_unbox(n_name(i)), n_int(1))));
    println_of(b, n_name(sum));
}

static void build_call(void)
{
    struct block *b = begin_main();
    int fact = add_var("fact", "func(any) any", F_MAIN, true, false);
    add_stmt(b, S_DECL, fact, false, NULL);

    /* The function literal. Go names the closures of main main.func1. */
    int fn = add_func("main.func1", "(n any) any", true);
    int n = add_var("n", "any", fn, true, false);
    int result = add_var("~r0", "any", fn, true, false);
    funcs[fn].param = n;
    funcs[fn].result = result;
    funcs[fn].captured = fact;
    struct block *body = funcs[fn].body;
    struct stmt *base = add_stmt(body, S_IF, 0, false, n_bin('<', n_unbox(n_name(n)), n_int(2)));
    add_stmt(base->body, S_RETURN, 0, false, n_box(n_int(1)));
    /* The callee is known: fact is assigned once, to this literal, and the compiler finds that
     * out (ir.ReassignOracle), so the call is analysed as a call of main.func1. */
    struct node *rec = n_call(fn, n_name(fact), n_box(n_bin('-', n_unbox(n_name(n)), n_int(1))));
    add_stmt(body, S_RETURN, 0, false,
             n_box(n_bin('*', n_unbox(n_name(n)), n_unbox(rec))));

    add_stmt(b, S_ASSIGN, fact, false, n_closure(fn));
    println_of(b, n_call(fn, n_name(fact), n_box(n_int(5))));
}

/* ------------------------------------------------------------------------------------------
 * Printing the tree as Go
 * ---------------------------------------------------------------------------------------- */

struct sbuf {
    char s[96];
    size_t n;
};

static void sb_add(struct sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void sb_add(struct sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->s + b->n, sizeof(b->s) - b->n, fmt, ap);
    va_end(ap);
    b->n += (size_t)w;
}

static int prec(const struct node *n)
{
    if (n->kind != N_BIN) {
        return 3;
    }
    return n->op == '*' ? 2 : 1;
}

/* source: print as in a listing (any(x), []any{x}, the closure's signature). Otherwise print as
 * the compiler names an expression in its messages. */
static void fmt_node(struct sbuf *b, const struct node *n, bool source)
{
    switch (n->kind) {
    case N_INT:
        sb_add(b, "%ld", n->val);
        break;
    case N_STR:
        sb_add(b, "\"%s\"", strings[n->val]);
        break;
    case N_NAME:
        sb_add(b, "%s", vars[n->var].name);
        break;
    case N_BIN:
        for (int side = 0; side < 2; side++) {
            const struct node *child = side == 0 ? n->x : n->y;
            bool paren = prec(child) < prec(n);
            if (paren) {
                sb_add(b, "(");
            }
            fmt_node(b, child, source);
            if (paren) {
                sb_add(b, ")");
            }
            if (side == 0) {
                sb_add(b, " %c ", n->op);
            }
        }
        break;
    case N_INDEX:
        fmt_node(b, n->x, source);
        sb_add(b, "[");
        fmt_node(b, n->y, source);
        sb_add(b, "]");
        break;
    case N_LEN:
        sb_add(b, "len(");
        fmt_node(b, n->x, source);
        sb_add(b, ")");
        break;
    case N_UNBOX:
        fmt_node(b, n->x, source);
        sb_add(b, ".(int)");
        break;
    case N_BOX:
        if (source) {
            sb_add(b, "any(");
        }
        fmt_node(b, n->x, source);
        if (source) {
            sb_add(b, ")");
        }
        break;
    case N_SLICE:
        sb_add(b, "[]any{");
        if (source) {
            fmt_node(b, n->x, source);
        } else {
            sb_add(b, "...");
        }
        sb_add(b, "}");
        break;
    case N_CLOSURE:
        if (source) {
            sb_add(b, "func%s {...}", funcs[n->fn].sig);
        } else {
            sb_add(b, "func literal");
        }
        break;
    default: /* N_CALL */
        if (n->x != NULL) {
            fmt_node(b, n->x, source);
        } else {
            sb_add(b, "%s", funcs[n->fn].name);
        }
        sb_add(b, "(");
        fmt_node(b, n->y, source);
        sb_add(b, ")");
        break;
    }
}

static void print_block(const struct block *b, int indent);

static void print_stmt(const struct stmt *s, int indent)
{
    struct sbuf e = {{0}, 0};
    if (s->e != NULL) {
        fmt_node(&e, s->e, true);
    }
    switch (s->kind) {
    case S_DECL:
        fprintf(stderr, "%*svar %s %s\n", indent, "", vars[s->var].name, vars[s->var].type);
        break;
    case S_ASSIGN:
        fprintf(stderr, "%*s%s %s %s\n", indent, "", vars[s->var].name, s->define ? ":=" : "=",
                e.s);
        break;
    case S_IF:
        fprintf(stderr, "%*sif %s {\n", indent, "", e.s);
        print_block(s->body, indent + 4);
        if (s->els->n > 0) {
            fprintf(stderr, "%*s} else {\n", indent, "");
            print_block(s->els, indent + 4);
        }
        fprintf(stderr, "%*s}\n", indent, "");
        break;
    case S_WHILE:
        fprintf(stderr, "%*sfor %s {\n", indent, "", e.s);
        print_block(s->body, indent + 4);
        fprintf(stderr, "%*s}\n", indent, "");
        break;
    case S_RETURN:
        fprintf(stderr, "%*sreturn %s\n", indent, "", e.s);
        break;
    case S_CALL:
        fprintf(stderr, "%*s%s\n", indent, "", e.s);
        break;
    default: /* S_PRINT */
        fprintf(stderr, "%*sprint(%s)\n", indent, "", e.s);
        break;
    }
}

static void print_block(const struct block *b, int indent)
{
    for (int i = 0; i < b->n; i++) {
        print_stmt(b->s[i], indent);
    }
}

/* ------------------------------------------------------------------------------------------
 * Escape analysis
 * ---------------------------------------------------------------------------------------- */

/* A location is a variable or the storage an expression allocates. */
struct edge {
    int src;
    int derefs;
};

struct loc {
    char name[112];
    int var;            /* the variable it stands for, or -1 */
    struct node *node;  /* the expression whose storage it is, or NULL */
    int func;           /* the function it belongs to */
    int depth;          /* loop depth where it is declared */
    struct edge edges[MAX_EDGES]; /* incoming: what is assigned to it */
    int nedges;
    int derefs;         /* during a walk: the least number of dereferences from the root */
    bool escapes, addrtaken, param, queued;
    unsigned leaks;     /* bit d: the parameter flows to the heap with d dereferences */
};

enum { LOC_HEAP, LOC_BLANK };

struct closure_rec {
    struct hole k; /* where the closure's address goes: its own storage */
    int fn;
};

static struct loc locs[MAX_LOCS];
static int nlocs;
static int var_loc[MAX_VARS];
static struct closure_rec closures[MAX_CLOSURES];
static int nclosures;
static int cur_fn, cur_depth, cur_batch;
static int queue[2 * MAX_LOCS];
static int nqueue;

static struct hole hole_of(int loc)
{
    return (struct hole){loc, 0, false};
}

/* Dereferencing adds, taking an address subtracts. Taking one marks the source as address-taken. */
static struct hole shift(struct hole k, int delta)
{
    k.derefs += delta;
    k.addrtaken = delta < 0;
    return k;
}

static int new_loc(const char *name, int var, struct node *node)
{
    struct loc *l = &locs[nlocs];
    memset(l, 0, sizeof(*l));
    snprintf(l->name, sizeof(l->name), "%s", name);
    l->var = var;
    l->node = node;
    l->func = cur_fn;
    l->depth = cur_depth;
    return nlocs++;
}

static int new_node_loc(struct node *n)
{
    struct sbuf text = {{0}, 0};
    fmt_node(&text, n, false);
    snprintf(n->text, sizeof(n->text), "%s", text.s);
    char name[112];
    snprintf(name, sizeof(name), "{storage for %s}", n->text);
    return new_loc(name, -1, n);
}

/* Records the assignment `dst = src` with the weight of the hole. */
static void flow(struct hole k, int src)
{
    if (k.addrtaken) {
        locs[src].addrtaken = true;
    }
    if (k.dst == LOC_BLANK) {
        return;
    }
    struct loc *dst = &locs[k.dst];
    dst->edges[dst->nedges++] = (struct edge){src, k.derefs};
}

/* Allocates the storage of an expression and flows its address to k. Returns the hole for
 * what is stored in it. */
static struct hole spill(struct hole k, struct node *n)
{
    int l = new_node_loc(n);
    flow(shift(k, -1), l);
    return hole_of(l);
}

static void expr(struct hole k, struct node *n);
static void stmt(const struct stmt *s);

static void discard(struct node *n)
{
    expr(hole_of(LOC_BLANK), n);
}

static void walk_func(int f);

static void call(struct hole k, struct node *n)
{
    if (n->x != NULL) {
        discard(n->x);
    }
    const struct func *callee = &funcs[n->fn];
    if (callee->batch == cur_batch) {
        /* A callee in the same batch has no tags yet: the argument flows into the parameter and
         * the result flows into the context of the call. */
        flow(k, var_loc[callee->result]);
        expr(hole_of(var_loc[callee->param]), n->y);
    } else {
        expr(callee->tag, n->y);
    }
}

/* Evaluates n and flows its value into the hole k. */
static void expr(struct hole k, struct node *n)
{
    if (!n->ptr) {
        k.dst = LOC_BLANK; /* a value without pointers cannot carry an address */
    }
    switch (n->kind) {
    case N_INT:
    case N_STR:
        break;
    case N_NAME:
        flow(k, var_loc[n->var]);
        break;
    case N_BIN:
        discard(n->x);
        discard(n->y);
        break;
    case N_LEN:
        discard(n->x);
        break;
    case N_INDEX:
        expr(shift(k, 1), n->x);
        discard(n->y);
        break;
    case N_UNBOX:
        expr(shift(k, 1), n->x);
        break;
    case N_BOX:
    case N_SLICE:
        expr(spill(k, n), n->x);
        break;
    case N_CLOSURE:
        closures[nclosures++] = (struct closure_rec){spill(k, n), n->fn};
        walk_func(n->fn);
        break;
    default: /* N_CALL */
        call(k, n);
        break;
    }
}

static void stmts(const struct block *b)
{
    for (int i = 0; i < b->n; i++) {
        stmt(b->s[i]);
    }
}

static void stmt(const struct stmt *s)
{
    switch (s->kind) {
    case S_DECL:
        locs[var_loc[s->var]].depth = cur_depth;
        break;
    case S_ASSIGN:
        if (s->define) {
            locs[var_loc[s->var]].depth = cur_depth;
        }
        /* An assignment to a global goes to the heap. */
        expr(vars[s->var].global ? hole_of(LOC_HEAP) : hole_of(var_loc[s->var]), s->e);
        break;
    case S_IF:
        discard(s->e);
        stmts(s->body);
        stmts(s->els);
        break;
    case S_WHILE:
        discard(s->e);
        cur_depth++;
        stmts(s->body);
        cur_depth--;
        break;
    case S_RETURN:
        expr(hole_of(var_loc[funcs[cur_fn].result]), s->e);
        break;
    case S_CALL:
        call(hole_of(LOC_BLANK), s->e);
        break;
    default: /* S_PRINT */
        discard(s->e);
        break;
    }
}

static void walk_func(int f)
{
    int saved_fn = cur_fn;
    int saved_depth = cur_depth;
    cur_fn = f;
    cur_depth = 1;
    stmts(funcs[f].body);
    cur_fn = saved_fn;
    cur_depth = saved_depth;
}

static const char *const ops[] = {"&", "", "*", "**"};

static void dump_graph(void)
{
    fprintf(stderr, "graph (each line is an assignment `dst = src`; & is an address taken):\n");
    for (int l = 0; l < nlocs; l++) {
        for (int e = 0; e < locs[l].nedges; e++) {
            const struct edge *edge = &locs[l].edges[e];
            fprintf(stderr, "  %s = %s%s\n", locs[l].name, ops[edge->derefs + 1],
                    locs[edge->src].name);
        }
    }
}

/* Whether what is stored in root may live longer than other, if other is on the stack. */
static bool outlives(int root, int other)
{
    const struct loc *r = &locs[root];
    const struct loc *o = &locs[other];
    return r->escapes || (r->func == o->func && r->depth < o->depth);
}

static void enqueue(int l)
{
    if (!locs[l].queued) {
        locs[l].queued = true;
        queue[nqueue++] = l;
    }
}

/* Walks the graph backwards from root. This is Bellman-Ford, because the edges of an address-of
 * are negative: the least number of dereferences from the root is kept for every location, and
 * every edge is relaxed until nothing changes. A count is never allowed below 0 when it goes on
 * through a location: in `p = &q; q = r`, the address of q reaches p, but the address of r does
 * not. */
static void walk(int root)
{
    fprintf(stderr, "  walk from %s\n", locs[root].name);
    for (int l = 0; l < nlocs; l++) {
        locs[l].derefs = UNREACHED;
    }
    locs[root].derefs = 0;
    for (bool changed = true; changed;) {
        changed = false;
        for (int dst = 0; dst < nlocs; dst++) {
            if (locs[dst].derefs == UNREACHED) {
                continue;
            }
            int base = locs[dst].derefs < 0 ? 0 : locs[dst].derefs;
            for (int e = 0; e < locs[dst].nedges; e++) {
                struct loc *src = &locs[locs[dst].edges[e].src];
                int derefs = base + locs[dst].edges[e].derefs;
                if (derefs < src->derefs) {
                    src->derefs = derefs;
                    changed = true;
                }
            }
        }
    }
    for (int l = 0; l < nlocs; l++) {
        struct loc *loc = &locs[l];
        if (loc->derefs == UNREACHED) {
            continue;
        }
        /* The address of loc reaches something that outlives it: loc is moved to the heap, and
         * from now on it is a root that outlives everything. */
        if (loc->derefs < 0 && outlives(root, l) && !loc->escapes) {
            loc->escapes = true;
            fprintf(stderr, "    %s escapes to the heap\n", loc->name);
            enqueue(l);
        }
        /* A parameter whose value reaches the heap is recorded for the callers. */
        if (loc->param && outlives(root, l)) {
            int derefs = loc->derefs < 0 ? 0 : loc->derefs;
            loc->leaks |= 1u << derefs;
            fprintf(stderr, "    parameter %s leaks to %s with derefs=%d\n", loc->name,
                    locs[root].name, derefs);
        }
    }
}

static void walk_all(void)
{
    fprintf(stderr, "walks (each location is a root; a location that escapes is walked again):\n");
    nqueue = 0;
    for (int l = 0; l < nlocs; l++) {
        if (l != LOC_BLANK) {
            enqueue(l);
        }
    }
    for (int head = 0; head < nqueue; head++) {
        locs[queue[head]].queued = false;
        walk(queue[head]);
    }
}

static const char *const leak_words[] = {"leaking param", "leaking param content"};
static const char *const verdicts[] = {"does not escape", "escapes to heap"};

/* Records the results: the decisions for the code that allocates, and a tag for each parameter. */
static void finish(int first, int end)
{
    fprintf(stderr, "results:\n");
    for (int f = first; f < end; f++) {
        struct func *fn = &funcs[f];
        if (fn->param < 0) {
            continue;
        }
        unsigned leaks = locs[var_loc[fn->param]].leaks;
        if (leaks != 0) {
            int derefs = __builtin_ctz(leaks);
            fn->tag = (struct hole){LOC_HEAP, derefs, false};
            fprintf(stderr, "  %s: %s: %s\n", fn->name, leak_words[derefs > 0],
                    vars[fn->param].name);
        } else {
            fn->tag = hole_of(LOC_BLANK);
            fprintf(stderr, "  %s: %s %s\n", fn->name, vars[fn->param].name, verdicts[0]);
        }
    }
    for (int l = 2; l < nlocs; l++) {
        if (locs[l].var >= 0) {
            vars[locs[l].var].heap = locs[l].escapes;
            continue;
        }
        locs[l].node->heap = locs[l].escapes;
        fprintf(stderr, "  %s %s\n", locs[l].node->text, verdicts[locs[l].escapes]);
    }
}

static void escape_batch(int batch, int first, int end)
{
    fprintf(stderr, "-- batch %d:", batch + 1);
    for (int f = first; f < end; f++) {
        fprintf(stderr, " %s", funcs[f].name);
    }
    fprintf(stderr, " --\n");

    nlocs = 0;
    nclosures = 0;
    cur_batch = batch;
    cur_depth = 1;
    new_loc("{heap}", -1, NULL);
    new_loc("_", -1, NULL);
    locs[LOC_HEAP].escapes = true;

    /* A location for every variable of every function in the batch. */
    for (int f = first; f < end; f++) {
        funcs[f].batch = batch;
        cur_fn = f;
        for (int v = 0; v < nvars; v++) {
            if (vars[v].func == f) {
                var_loc[v] = new_loc(vars[v].name, v, NULL);
                locs[var_loc[v]].param = v == funcs[f].param;
                /* The compiler assumes that what a function returns outlives it. */
                locs[var_loc[v]].escapes = v == funcs[f].result;
            }
        }
    }
    /* Walk the bodies. A function literal is walked where it appears, inside its function. */
    for (int f = first; f < end; f++) {
        if (!funcs[f].closure) {
            walk_func(f);
        }
    }
    /* Now it is known which closures exist: flow the variables they capture into them. A
     * variable that is assigned after it is declared is captured by reference. */
    for (int c = 0; c < nclosures; c++) {
        flow(shift(closures[c].k, -1), var_loc[funcs[closures[c].fn].captured]);
    }
    dump_graph();
    walk_all();
    finish(first, end);
}

/* ------------------------------------------------------------------------------------------
 * Running the program
 * ---------------------------------------------------------------------------------------- */

struct frame {
    long cell[MAX_VARS]; /* the address of each variable */
    long ret;
    bool returned;
};

static long stack_mem[STACK_CELLS];
static long heap_mem[HEAP_CELLS];
static long sp = 1;       /* the first free cell of the stack, 0 is no address */
static long heap_top = 1; /* the first free cell of the heap, cell 0 is `sink` */
static long peak_sp = 1;
static int heap_allocs;

/* A pointer to a frame that has returned is the bug that escape analysis exists to prevent. */
static void check_live(long addr)
{
    /* GCOVR_EXCL_START: only if the analysis left an address of a dead frame in live data */
    if (addr < HEAP_BASE && addr >= sp) {
        fprintf(stderr, "dangling pointer into a stack frame that has returned\n");
        exit(1);
    }
    /* GCOVR_EXCL_STOP */
}

static long load(long addr)
{
    check_live(addr);
    return addr >= HEAP_BASE ? heap_mem[addr - HEAP_BASE] : stack_mem[addr];
}

static void store(long addr, long value)
{
    check_live(addr);
    if (addr >= HEAP_BASE) {
        heap_mem[addr - HEAP_BASE] = value;
    } else {
        stack_mem[addr] = value;
    }
}

static long alloc(bool heap, int cells, const char *name)
{
    long addr;
    if (heap) {
        addr = HEAP_BASE + heap_top;
        heap_top += cells;
        heap_allocs++;
    } else {
        addr = sp;
        sp += cells;
        peak_sp = sp > peak_sp ? sp : peak_sp;
    }
    fprintf(stderr, "  alloc %s: %s\n", name, heap ? "heap" : "stack");
    return addr;
}

static long eval(struct frame *f, const struct node *n);
static void exec_block(struct frame *f, const struct block *b);

static long call_func(int fn, long arg, long closure)
{
    const struct func *callee = &funcs[fn];
    struct frame frame;
    frame.ret = 0;
    frame.returned = false;
    long saved = sp;
    if (callee->param >= 0) {
        const struct var *param = &vars[callee->param];
        frame.cell[callee->param] = alloc(param->heap, 1, param->name);
        store(frame.cell[callee->param], arg);
    }
    if (callee->captured >= 0) {
        frame.cell[callee->captured] = load(closure + 1);
    }
    exec_block(&frame, callee->body);
    sp = saved; /* the frame is gone: whatever lived on the stack is dead */
    return frame.ret;
}

static long eval(struct frame *f, const struct node *n)
{
    switch (n->kind) {
    case N_INT:
    case N_STR:
        return n->val;
    case N_NAME:
        return load(f->cell[n->var]);
    case N_BIN: {
        long x = eval(f, n->x);
        long y = eval(f, n->y);
        switch (n->op) {
        case '+':
            return x + y;
        case '-':
            return x - y;
        case '*':
            return x * y;
        default: /* '<' */
            return x < y;
        }
    }
    case N_INDEX: {
        long slice = eval(f, n->x);
        return load(slice + 1 + eval(f, n->y));
    }
    case N_LEN:
        return load(eval(f, n->x));
    case N_UNBOX:
        return load(eval(f, n->x) + 1);
    case N_BOX:
    case N_SLICE: {
        /* A box is [is_string, value]. A slice is [length, element]. */
        long value = eval(f, n->x);
        long p = alloc(n->heap, 2, n->text);
        store(p, n->kind == N_BOX ? n->val : 1);
        store(p + 1, value);
        return p;
    }
    case N_CLOSURE: {
        /* A closure is [function, address of the captured variable]. */
        long p = alloc(n->heap, 2, n->text);
        store(p, n->fn);
        store(p + 1, f->cell[funcs[n->fn].captured]);
        return p;
    }
    default: { /* N_CALL */
        long closure = 0;
        if (n->x != NULL) {
            closure = eval(f, n->x);
        }
        return call_func(n->fn, eval(f, n->y), closure);
    }
    }
}

static void exec_stmt(struct frame *f, const struct stmt *s)
{
    switch (s->kind) {
    case S_DECL:
        f->cell[s->var] = alloc(vars[s->var].heap, 1, vars[s->var].name);
        break;
    case S_ASSIGN: {
        long value = eval(f, s->e);
        if (s->define) {
            f->cell[s->var] = alloc(vars[s->var].heap, 1, vars[s->var].name);
        }
        store(vars[s->var].global ? SINK_ADDR : f->cell[s->var], value);
        break;
    }
    case S_IF:
        exec_block(f, eval(f, s->e) ? s->body : s->els);
        break;
    case S_WHILE:
        while (eval(f, s->e)) {
            exec_block(f, s->body);
        }
        break;
    case S_RETURN:
        f->ret = eval(f, s->e);
        f->returned = true;
        break;
    case S_CALL:
        eval(f, s->e);
        break;
    default: { /* S_PRINT */
        long box = eval(f, s->e);
        if (load(box) == 1) {
            printf("%s\n", strings[load(box + 1)]);
        } else {
            printf("%ld\n", load(box + 1));
        }
        break;
    }
    }
}

static void exec_block(struct frame *f, const struct block *b)
{
    for (int i = 0; i < b->n && !f->returned; i++) {
        exec_stmt(f, b->s[i]);
    }
}

/* ------------------------------------------------------------------------------------------
 * Stages
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
    fprintf(stderr, "== stage_build: the typed tree, as Go ==\n");
    fprintf(stderr, "var sink any\n");
    for (int f = 0; f < nfuncs; f++) {
        fprintf(stderr, "\nfunc %s%s {\n", funcs[f].name, funcs[f].sig);
        print_block(funcs[f].body, 4);
        fprintf(stderr, "}\n");
    }
}

static void stage_escape(void)
{
    fprintf(stderr, "== stage_escape: batches of functions, callees first ==\n");
    int bounds[] = {0, 1, 2, nfuncs};
    for (int batch = 0; batch < 3; batch++) {
        escape_batch(batch, bounds[batch], bounds[batch + 1]);
    }
}

static void stage_run(void)
{
    fprintf(stderr, "== stage_run: every allocation, where the analysis put it ==\n");
    call_func(F_MAIN, 0, 0);
    fprintf(stderr, "heap allocations: %d, deepest stack: %ld cells\n", heap_allocs, peak_sp - 1);
}

int main(int argc, char **argv)
{
    const struct example *example = NULL;
    if (argc < 2 || (example = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_build(example);
    stage_escape();
    stage_run();
    return 0;
}
