/*
 * ghc: a cut-down version of the back half of the Glasgow Haskell Compiler, the STG language
 * and the machine that runs it.
 *
 * GHC compiles a lazy language. Its last intermediate form, STG, is a tiny functional
 * language in which every argument is an atom (a variable or a literal), every allocation is
 * a `let`, and every evaluation is a `case`. Each closure that `let` allocates says how it
 * behaves when entered: a function (`\r`) can be entered again and again, a thunk (`\u`) is
 * entered once, then overwritten with its value, and a join point (`\j`, a let-no-escape)
 * never becomes a heap object at all, because it is only ever jumped to. The machine that
 * runs STG has two kinds of stack frame: a case continuation, pushed when `case` evaluates
 * its scrutinee, and an update frame, pushed when a thunk is entered, which writes the thunk's
 * value back into it when the value arrives.
 *
 * This entry skips Haskell, the type checker and the Core simplifier. The STG program of each
 * example is built by hand, the way GHC's CoreToStg would hand it over. The decision of each
 * closure's update flag, the free-variable annotation and the machine are the real design in
 * small. The machine is an interpreter in place of the Cmm code generator, so the heap and
 * the stack are plain arrays.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. The machine interprets STG and generates no machine code, so the code.bin
 *   argument is ignored.
 *
 * Stages:
 *   stage_source  build the STG program of the example and print it (stands in for the
 *                 front end, the simplifier and CoreToStg)
 *   stage_flags   turn a constructor application into a constructor closure and give every
 *                 other closure its update flag (mkStgRhs)
 *   stage_fvs     annotate every closure with the variables it captures (GHC.Stg.FVs)
 *   stage_run     run the program on the machine, printing every allocation, call, jump,
 *                 thunk entry and update
 */
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------
 * The STG syntax: atoms, expressions, alternatives and bindings
 * ---------------------------------------------------------------------------------------- */

enum atom_kind { A_VAR, A_INT, A_STR };

struct atom {
    enum atom_kind kind;
    const char *name; /* a variable's name, or a string literal's text */
    long num;
};

enum expr_kind { E_APP, E_CON, E_OP, E_CASE, E_LET };

struct alt;
struct bind;

struct expr {
    enum expr_kind kind;
    const char *name; /* the function of an application, a constructor, a primop, a case binder */
    int nargs;
    const struct atom *args;
    const struct expr *scrut;
    int nalts;
    const struct alt *alts;
    int let_no_escape;
    int nbinds;
    struct bind *binds;
    const struct expr *body; /* the body of a let */
};

enum alt_kind { ALT_CON, ALT_LIT, ALT_DEFAULT };

struct alt {
    enum alt_kind kind;
    const char *con;
    const char *var; /* the field bound by a constructor alternative */
    long lit;
    const struct expr *body;
};

#define MAX_FREE 8

struct bind {
    const char *name;
    int nparams;
    const char *const *params;
    const struct expr *body;
    char flag; /* 'r' function, 'u' updatable thunk, 'j' join point, 0 until stage_flags */
    int is_con; /* a constructor closure: the closure is its body, which is a constructor */
    int nfree;
    const char *free[MAX_FREE];
};

struct program {
    int ntop;
    struct bind top[1];
    const struct expr *main;
};

/* ------------------------------------------------------------------------------------------
 * Memory for the syntax tree: one static arena, so that there is nothing to free
 * ---------------------------------------------------------------------------------------- */

static union {
    char bytes[1 << 15];
    long align;
} arena;
static size_t arena_used;

static void *alloc(size_t size)
{
    void *p = arena.bytes + arena_used;
    arena_used += (size + 15) & ~(size_t)15;
    return p;
}

static void *copy(const void *src, size_t size)
{
    return size ? memcpy(alloc(size), src, size) : NULL;
}

/* ------------------------------------------------------------------------------------------
 * Constructor functions for building a program by hand
 * ---------------------------------------------------------------------------------------- */

#define V(s) ((struct atom){A_VAR, s, 0})
#define I(n) ((struct atom){A_INT, NULL, n})
#define S(s) ((struct atom){A_STR, s, 0})
#define ARGS(...) (int)(sizeof((struct atom[]){__VA_ARGS__}) / sizeof(struct atom)), (struct atom[]){__VA_ARGS__}
#define NOARGS 0, NULL
#define PARAMS(...) (int)(sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *)), (const char *[]){__VA_ARGS__}
#define NOPARAMS 0, NULL
#define ALTS(...) (int)(sizeof((struct alt[]){__VA_ARGS__}) / sizeof(struct alt)), (struct alt[]){__VA_ARGS__}
#define BINDS(...) (int)(sizeof((struct bind[]){__VA_ARGS__}) / sizeof(struct bind)), (struct bind[]){__VA_ARGS__}

static struct expr *new_expr(enum expr_kind kind, const char *name)
{
    struct expr *e = alloc(sizeof *e);
    *e = (struct expr){.kind = kind, .name = name};
    return e;
}

static const struct expr *e_atoms(enum expr_kind kind, const char *name, int n, const struct atom *args)
{
    struct expr *e = new_expr(kind, name);
    e->nargs = n;
    e->args = copy(args, (size_t)n * sizeof *args);
    return e;
}

static const struct expr *e_app(const char *f, int n, const struct atom *args)
{
    return e_atoms(E_APP, f, n, args);
}

static const struct expr *e_con(const char *con, int n, const struct atom *args)
{
    return e_atoms(E_CON, con, n, args);
}

static const struct expr *e_op(const char *op, int n, const struct atom *args)
{
    return e_atoms(E_OP, op, n, args);
}

static const struct expr *e_case(const struct expr *scrut, const char *binder, int n, const struct alt *alts)
{
    struct expr *e = new_expr(E_CASE, binder);
    e->scrut = scrut;
    e->nalts = n;
    e->alts = copy(alts, (size_t)n * sizeof *alts);
    return e;
}

static const struct expr *e_let(int let_no_escape, int n, const struct bind *binds, const struct expr *body)
{
    struct expr *e = new_expr(E_LET, NULL);
    e->let_no_escape = let_no_escape;
    e->nbinds = n;
    e->binds = copy(binds, (size_t)n * sizeof *binds);
    e->body = body;
    return e;
}

static struct bind b_clo(const char *name, int n, const char *const *params, const struct expr *body)
{
    return (struct bind){.name = name, .nparams = n, .params = copy(params, (size_t)n * sizeof *params), .body = body};
}

static struct alt a_con(const char *con, const char *var, const struct expr *body)
{
    return (struct alt){.kind = ALT_CON, .con = con, .var = var, .body = body};
}

static struct alt a_lit(long lit, const struct expr *body)
{
    return (struct alt){.kind = ALT_LIT, .lit = lit, .body = body};
}

static struct alt a_default(const struct expr *body)
{
    return (struct alt){.kind = ALT_DEFAULT, .body = body};
}

/* case scrut of _ { DEFAULT -> body }, which GHC writes to force one primop result */
static const struct expr *e_prim(const char *binder, const struct expr *scrut, const struct expr *body)
{
    return e_case(scrut, binder, ALTS(a_default(body)));
}

/* ------------------------------------------------------------------------------------------
 * Printing in the style of GHC's STG dumps
 * ---------------------------------------------------------------------------------------- */

static void print_atom(const struct atom *a)
{
    switch (a->kind) {
    case A_VAR:
        fprintf(stderr, "%s", a->name);
        break;
    case A_INT:
        fprintf(stderr, "%ld#", a->num);
        break;
    default:
        fprintf(stderr, "\"%s\"", a->name);
    }
}

static void print_atoms(int n, const struct atom *args)
{
    for (int i = 0; i < n; i++) {
        fprintf(stderr, i ? " " : "");
        print_atom(&args[i]);
    }
}

static void print_expr(const struct expr *e, int indent, int show_free);

static void print_bind(const struct bind *b, int indent, int show_free)
{
    fprintf(stderr, "%*s%s = ", indent, "", b->name);
    if (b->is_con) {
        fprintf(stderr, "%s [", b->body->name);
        print_atoms(b->body->nargs, b->body->args);
        fprintf(stderr, "]\n");
        return;
    }
    fprintf(stderr, "\\%c", b->flag ? b->flag : ' ');
    if (show_free && b->flag != 'j') {
        fprintf(stderr, " {");
        for (int i = 0; i < b->nfree; i++) {
            fprintf(stderr, "%s%s", i ? " " : "", b->free[i]);
        }
        fprintf(stderr, "}");
    }
    fprintf(stderr, " [");
    for (int i = 0; i < b->nparams; i++) {
        fprintf(stderr, "%s%s", i ? " " : "", b->params[i]);
    }
    fprintf(stderr, "] ->\n");
    print_expr(b->body, indent + 4, show_free);
}

static void print_simple(const struct expr *e)
{
    if (e->kind == E_APP) {
        fprintf(stderr, "%s%s", e->name, e->nargs ? " " : "");
        print_atoms(e->nargs, e->args);
    } else {
        fprintf(stderr, "%s [", e->name);
        print_atoms(e->nargs, e->args);
        fprintf(stderr, "]");
    }
}

static void print_expr(const struct expr *e, int indent, int show_free)
{
    switch (e->kind) {
    case E_APP:
    case E_CON:
    case E_OP:
        fprintf(stderr, "%*s", indent, "");
        print_simple(e);
        fprintf(stderr, "\n");
        break;
    case E_CASE:
        fprintf(stderr, "%*scase ", indent, "");
        print_simple(e->scrut);
        fprintf(stderr, " of %s {\n", e->name);
        for (int i = 0; i < e->nalts; i++) {
            const struct alt *a = &e->alts[i];
            if (a->kind == ALT_CON) {
                fprintf(stderr, "%*s%s %s ->\n", indent + 2, "", a->con, a->var);
            } else if (a->kind == ALT_LIT) {
                fprintf(stderr, "%*s%ld# ->\n", indent + 2, "", a->lit);
            } else {
                fprintf(stderr, "%*sDEFAULT ->\n", indent + 2, "");
            }
            print_expr(a->body, indent + 4, show_free);
        }
        fprintf(stderr, "%*s}\n", indent, "");
        break;
    default:
        fprintf(stderr, "%*s%s {\n", indent, "", e->let_no_escape ? "let-no-escape" : "let");
        for (int i = 0; i < e->nbinds; i++) {
            print_bind(&e->binds[i], indent + 2, show_free);
        }
        fprintf(stderr, "%*s} in\n", indent, "");
        print_expr(e->body, indent + 2, show_free);
    }
}

static void print_program(const struct program *p, int show_free)
{
    for (int i = 0; i < p->ntop; i++) {
        print_bind(&p->top[i], 0, show_free);
    }
    fprintf(stderr, "main =\n");
    print_expr(p->main, 4, show_free);
}

/* ------------------------------------------------------------------------------------------
 * stage_source: the STG program of each example
 *
 * Every Int is the boxed constructor I# around an Int#, so evaluating a number means a case
 * on it. A comparison returns 1# or 0# and is scrutinized with a literal case. `print` is
 * the primop putInt# (or putStr#) on the unboxed number, applied last.
 * ---------------------------------------------------------------------------------------- */

static struct program program;

static const struct expr *box(const char *name, const char *prim_op, struct atom a, struct atom b)
{
    return e_prim(name, e_op(prim_op, ARGS(a, b)), e_con("I#", ARGS(V(name))));
}

static struct program *stage_source(const char *example)
{
    if (strcmp(example, "hello") == 0) {
        program.main = e_op("putStr#", ARGS(S("Hello, world!")));
    } else if (strcmp(example, "arithmetic") == 0) {
        /* (2 + 3) * 4 - 6, three thunks that each force the one before */
        const struct expr *sum = box("s", "+#", I(2), I(3));
        const struct expr *product = e_case(e_app("t1", NOARGS), "_", ALTS(a_con("I#", "a", box("m", "*#", V("a"), I(4)))));
        const struct expr *diff = e_case(e_app("t2", NOARGS), "_", ALTS(a_con("I#", "b", box("d", "-#", V("b"), I(6)))));
        const struct expr *show = e_case(e_app("t3", NOARGS), "_", ALTS(a_con("I#", "r", e_op("putInt#", ARGS(V("r"))))));
        program.main = e_let(0, BINDS(b_clo("t1", NOPARAMS, sum)),
            e_let(0, BINDS(b_clo("t2", NOPARAMS, product)),
                e_let(0, BINDS(b_clo("t3", NOPARAMS, diff)), show)));
    } else if (strcmp(example, "branch") == 0) {
        const struct expr *show_larger = e_case(e_op("<#", ARGS(V("x"), V("y"))), "_",
            ALTS(a_lit(1, e_op("putInt#", ARGS(V("y")))), a_default(e_op("putInt#", ARGS(V("x"))))));
        program.main = e_let(0, BINDS(b_clo("a", NOPARAMS, e_con("I#", ARGS(I(3)))), b_clo("b", NOPARAMS, e_con("I#", ARGS(I(5))))),
            e_case(e_app("a", NOARGS), "_", ALTS(a_con("I#", "x",
                e_case(e_app("b", NOARGS), "_", ALTS(a_con("I#", "y", show_larger)))))));
    } else if (strcmp(example, "call") == 0) {
        /* fact n = if n < 2 then 1 else n * fact (n - 1). Without optimization every use of n
         * is a new `case n`, and the argument n - 1 is passed as a thunk. */
        const struct expr *minus = e_case(e_app("n", NOARGS), "_", ALTS(a_con("I#", "k", box("d", "-#", V("k"), I(1)))));
        const struct expr *times = e_case(e_app("n", NOARGS), "_", ALTS(a_con("I#", "n2", box("p", "*#", V("n2"), V("r")))));
        const struct expr *recurse = e_let(0, BINDS(b_clo("m", NOPARAMS, minus)),
            e_case(e_app("fact", ARGS(V("m"))), "_", ALTS(a_con("I#", "r", times))));
        const struct expr *body = e_case(e_app("n", NOARGS), "_",
            ALTS(a_con("I#", "n1", e_case(e_op("<#", ARGS(V("n1"), I(2))), "_",
                ALTS(a_lit(1, e_con("I#", ARGS(I(1)))), a_default(recurse))))));
        program.ntop = 1;
        program.top[0] = b_clo("fact", PARAMS("n"), body);
        program.main = e_let(0, BINDS(b_clo("five", NOPARAMS, e_con("I#", ARGS(I(5))))),
            e_case(e_app("fact", ARGS(V("five"))), "_",
                ALTS(a_con("I#", "r", e_op("putInt#", ARGS(V("r")))))));
    } else if (strcmp(example, "loop") == 0) {
        /* a loop is a join point: it is only ever tail called, so it needs no closure. The
         * accumulator and the counter are passed as thunks, which is what lazy code looks like
         * before strictness analysis has run. */
        const struct expr *add = e_case(e_app("sum", NOARGS), "_", ALTS(a_con("I#", "x",
            e_case(e_app("i", NOARGS), "_", ALTS(a_con("I#", "i2", box("r1", "+#", V("x"), V("i2"))))))));
        const struct expr *next = e_case(e_app("i", NOARGS), "_", ALTS(a_con("I#", "i3", box("r2", "+#", V("i3"), I(1)))));
        const struct expr *step = e_let(0, BINDS(b_clo("sum2", NOPARAMS, add), b_clo("inext", NOPARAMS, next)),
            e_app("loop", ARGS(V("sum2"), V("inext"))));
        const struct expr *done = e_case(e_app("sum", NOARGS), "_", ALTS(a_con("I#", "y", e_op("putInt#", ARGS(V("y"))))));
        const struct expr *body = e_case(e_app("i", NOARGS), "_",
            ALTS(a_con("I#", "i1", e_case(e_op("<#", ARGS(V("i1"), I(11))), "_",
                ALTS(a_lit(1, step), a_default(done))))));
        program.main = e_let(0, BINDS(b_clo("sum0", NOPARAMS, e_con("I#", ARGS(I(0)))), b_clo("i0", NOPARAMS, e_con("I#", ARGS(I(1))))),
            e_let(1, BINDS(b_clo("loop", PARAMS("sum", "i"), body)), e_app("loop", ARGS(V("sum0"), V("i0")))));
    } else {
        return NULL;
    }
    fprintf(stderr, "== stage_source: STG program of %s ==\n", example);
    print_program(&program, 0);
    return &program;
}

/* ------------------------------------------------------------------------------------------
 * stage_flags: what CoreToStg's mkStgRhs decides for every binding
 *
 * A zero-parameter binding whose body is a constructor applied to atoms is not code at all.
 * It becomes a constructor closure, which is already a value. Every other binding is code: a
 * join point is `\j`, a function is `\r`, and a thunk is `\u`. (GHC also has `\s`, a thunk
 * that demand analysis proved is entered at most once, which never needs an update.)
 * ---------------------------------------------------------------------------------------- */

static void flags_expr(const struct expr *e);

static void flags_bind(struct bind *b, int is_join)
{
    b->is_con = !is_join && b->nparams == 0 && b->body->kind == E_CON;
    b->flag = is_join ? 'j' : b->nparams > 0 ? 'r' : 'u';
    flags_expr(b->body);
}

static void flags_expr(const struct expr *e)
{
    switch (e->kind) {
    case E_CASE:
        flags_expr(e->scrut);
        for (int i = 0; i < e->nalts; i++) {
            flags_expr(e->alts[i].body);
        }
        break;
    case E_LET:
        for (int i = 0; i < e->nbinds; i++) {
            flags_bind(&e->binds[i], e->let_no_escape);
        }
        flags_expr(e->body);
        break;
    default:
        break;
    }
}

static void stage_flags(struct program *p)
{
    for (int i = 0; i < p->ntop; i++) {
        flags_bind(&p->top[i], 0);
    }
    flags_expr(p->main);
    fprintf(stderr, "== stage_flags: update flags and constructor closures ==\n");
    print_program(p, 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_fvs: the variables each closure captures (GHC.Stg.FVs)
 *
 * A closure copies the values of its free variables when it is allocated. Top-level names are
 * not free variables, since they have a fixed address, and neither are join points, since
 * they are jumped to and never captured.
 * ---------------------------------------------------------------------------------------- */

struct set {
    const char *name[MAX_FREE];
    int count;
};

static const struct program *fv_program;
static const char *join_names[4];
static int njoin_names;

static int is_member(const char *const *names, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void set_add(struct set *s, const char *name)
{
    if (!is_member(s->name, s->count, name)) {
        s->name[s->count++] = name;
    }
}

static void set_delete(struct set *s, const char *name)
{
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->name[i], name) == 0) {
            memmove(&s->name[i], &s->name[i + 1], (size_t)(s->count - i - 1) * sizeof s->name[0]);
            s->count--;
            return;
        }
    }
}

static void set_union(struct set *s, const struct set *other)
{
    for (int i = 0; i < other->count; i++) {
        set_add(s, other->name[i]);
    }
}

static void use(struct set *s, const char *name)
{
    int is_top = 0;
    for (int i = 0; i < fv_program->ntop; i++) {
        is_top |= strcmp(fv_program->top[i].name, name) == 0;
    }
    if (!is_top && !is_member(join_names, njoin_names, name)) {
        set_add(s, name);
    }
}

static void fv_expr(const struct expr *e, struct set *s);

static void fv_bind(struct bind *b, struct set *out)
{
    struct set own = {{NULL}, 0};
    if (b->is_con) {
        fv_expr(b->body, &own);
    } else {
        fv_expr(b->body, &own);
        for (int i = 0; i < b->nparams; i++) {
            set_delete(&own, b->params[i]);
        }
    }
    b->nfree = own.count;
    memcpy(b->free, own.name, sizeof own.name);
    set_union(out, &own);
}

static void fv_expr(const struct expr *e, struct set *s)
{
    switch (e->kind) {
    case E_CASE:
        fv_expr(e->scrut, s);
        for (int i = 0; i < e->nalts; i++) {
            struct set inner = {{NULL}, 0};
            fv_expr(e->alts[i].body, &inner);
            if (e->alts[i].kind == ALT_CON) {
                set_delete(&inner, e->alts[i].var);
            }
            set_delete(&inner, e->name);
            set_union(s, &inner);
        }
        break;
    case E_LET: {
        struct set inner = {{NULL}, 0};
        for (int i = 0; i < e->nbinds && e->let_no_escape; i++) {
            join_names[njoin_names++] = e->binds[i].name;
        }
        for (int i = 0; i < e->nbinds; i++) {
            fv_bind(&e->binds[i], &inner);
        }
        fv_expr(e->body, &inner);
        for (int i = 0; i < e->nbinds; i++) {
            set_delete(&inner, e->binds[i].name);
        }
        set_union(s, &inner);
        break;
    }
    default:
        if (e->kind == E_APP) {
            use(s, e->name);
        }
        for (int i = 0; i < e->nargs; i++) {
            if (e->args[i].kind == A_VAR) {
                use(s, e->args[i].name);
            }
        }
    }
}

static void stage_fvs(struct program *p)
{
    struct set unused = {{NULL}, 0};
    fv_program = p;
    for (int i = 0; i < p->ntop; i++) {
        fv_bind(&p->top[i], &unused);
    }
    fv_expr(p->main, &unused);
    fprintf(stderr, "== stage_fvs: variables captured by every closure ==\n");
    print_program(p, 1);
}

/* ------------------------------------------------------------------------------------------
 * stage_run: the machine
 *
 * The machine is always in one of two modes. In eval mode it runs an expression in an
 * environment. In return mode it has a value and hands it to the frame on top of the stack.
 * Only two things ever go on the stack: a case frame, which remembers the alternatives and the
 * environment to resume in, and an update frame, which remembers which thunk to overwrite.
 * A function call, a join point jump and a `let` push nothing.
 * ---------------------------------------------------------------------------------------- */

struct val {
    int is_addr; /* a heap address, or an unboxed number */
    long num;
};

#define MAX_ENV 16

struct env {
    int count;
    const char *name[MAX_ENV];
    struct val val[MAX_ENV];
};

enum closure_kind { K_FUN, K_THUNK, K_CON, K_IND };

struct closure {
    enum closure_kind kind;
    const struct bind *bind;
    struct env free; /* the captured variables of a function or thunk */
    const char *con;
    struct val field; /* the one field of a constructor */
    struct val target; /* what an updated thunk points to */
};

struct frame {
    int is_update;
    const struct expr *expr; /* the case expression to resume */
    struct env env;
    long updatee;
};

struct join {
    const struct bind *bind;
    struct env env;
};

static struct closure heap[256];
static long heap_top = 1;
static struct frame stack[64];
static int sp;
static struct env globals;
static struct join joins[4];
static int njoins;

static void env_push(struct env *env, const char *name, struct val v)
{
    env->name[env->count] = name;
    env->val[env->count++] = v;
}

static struct val lookup(const struct env *env, const char *name)
{
    const struct env *chain[2] = {env, &globals};
    for (int k = 0;; k++) {
        for (int i = chain[k]->count - 1; i >= 0; i--) {
            if (strcmp(chain[k]->name[i], name) == 0) {
                return chain[k]->val[i];
            }
        }
    }
}

static struct val atom_val(const struct env *env, const struct atom *a)
{
    return a->kind == A_VAR ? lookup(env, a->name) : (struct val){0, a->num};
}

static struct val deref(struct val v)
{
    while (heap[v.num].kind == K_IND) {
        v = heap[v.num].target;
    }
    return v;
}

static void print_val(struct val v)
{
    fprintf(stderr, v.is_addr ? "#%ld" : "%ld#", v.num);
}

static void print_values(const struct env *env, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "%s%s=", i ? " " : "", names[i]);
        print_val(lookup(env, names[i]));
    }
}

static void allocate(int n, struct bind *binds, struct env *env)
{
    long first = heap_top;
    for (int i = 0; i < n; i++) {
        env_push(env, binds[i].name, (struct val){1, heap_top++});
    }
    for (int i = 0; i < n; i++) {
        const struct bind *b = &binds[i];
        struct closure *c = &heap[first + i];
        fprintf(stderr, "  alloc #%ld = ", first + i);
        if (b->is_con) {
            c->kind = K_CON;
            c->con = b->body->name;
            c->field = atom_val(env, &b->body->args[0]);
            fprintf(stderr, "%s ", c->con);
            print_val(c->field);
        } else {
            c->kind = b->nparams > 0 ? K_FUN : K_THUNK;
            c->bind = b;
            for (int k = 0; k < b->nfree; k++) {
                env_push(&c->free, b->free[k], lookup(env, b->free[k]));
            }
            fprintf(stderr, "%s %s {", c->kind == K_FUN ? "FUN" : "THUNK", b->name);
            print_values(env, b->free, b->nfree);
            fprintf(stderr, "}");
        }
        fprintf(stderr, "\n");
    }
}

static struct env bind_params(const struct env *base, const struct bind *b, const struct val *args)
{
    struct env env = *base;
    for (int i = 0; i < b->nparams; i++) {
        env_push(&env, b->params[i], args[i]);
    }
    return env;
}

static void stage_run(struct program *p)
{
    const struct expr *e = p->main;
    struct env env = {0, {NULL}, {{0, 0}}};
    struct val ret = {0, 0};
    int returning = 0;

    fprintf(stderr, "== stage_run: allocations, calls, jumps, thunk entries and updates ==\n");
    allocate(p->ntop, p->top, &globals);
    for (;;) {
        if (returning) {
            if (sp == 0) {
                break;
            }
            const struct frame *f = &stack[--sp];
            if (f->is_update) {
                heap[f->updatee].kind = K_IND;
                heap[f->updatee].target = ret;
                fprintf(stderr, "  update #%ld := indirection to #%ld, stack %d\n", f->updatee, ret.num, sp);
                continue;
            }
            env = f->env;
            env_push(&env, f->expr->name, ret);
            int k = 0;
            while (f->expr->alts[k].kind == ALT_LIT && f->expr->alts[k].lit != ret.num) {
                k++;
            }
            const struct alt *a = &f->expr->alts[k];
            if (a->kind == ALT_CON) {
                env_push(&env, a->var, heap[ret.num].field);
            }
            e = a->body;
            returning = 0;
            continue;
        }
        switch (e->kind) {
        case E_LET:
            if (e->let_no_escape) {
                for (int i = 0; i < e->nbinds; i++) {
                    joins[njoins].bind = &e->binds[i];
                    joins[njoins++].env = env;
                    fprintf(stderr, "  join point %s, no closure\n", e->binds[i].name);
                }
            } else {
                allocate(e->nbinds, e->binds, &env);
            }
            e = e->body;
            break;
        case E_CASE:
            stack[sp].is_update = 0;
            stack[sp].expr = e;
            stack[sp++].env = env;
            e = e->scrut;
            break;
        case E_CON:
            heap[heap_top].kind = K_CON;
            heap[heap_top].con = e->name;
            heap[heap_top].field = atom_val(&env, &e->args[0]);
            fprintf(stderr, "  alloc #%ld = %s ", heap_top, e->name);
            print_val(heap[heap_top].field);
            fprintf(stderr, "\n");
            ret = (struct val){1, heap_top++};
            returning = 1;
            break;
        case E_OP:
            ret = (struct val){0, 0};
            switch (e->name[0]) {
            case 'p':
                if (e->args[0].kind == A_STR) {
                    printf("%s\n", e->args[0].name);
                } else {
                    printf("%ld\n", atom_val(&env, &e->args[0]).num);
                }
                break;
            case '+':
                ret.num = atom_val(&env, &e->args[0]).num + atom_val(&env, &e->args[1]).num;
                break;
            case '-':
                ret.num = atom_val(&env, &e->args[0]).num - atom_val(&env, &e->args[1]).num;
                break;
            case '*':
                ret.num = atom_val(&env, &e->args[0]).num * atom_val(&env, &e->args[1]).num;
                break;
            default:
                ret.num = atom_val(&env, &e->args[0]).num < atom_val(&env, &e->args[1]).num;
            }
            returning = 1;
            break;
        default: {
            struct val args[2];
            for (int i = 0; i < e->nargs; i++) {
                args[i] = atom_val(&env, &e->args[i]);
            }
            int j = 0;
            while (j < njoins && strcmp(joins[j].bind->name, e->name) != 0) {
                j++;
            }
            if (j < njoins) {
                fprintf(stderr, "  jump %s, stack %d\n", e->name, sp);
                env = bind_params(&joins[j].env, joins[j].bind, args);
                e = joins[j].bind->body;
                break;
            }
            struct val fn = deref(lookup(&env, e->name));
            const struct closure *c = &heap[fn.num];
            switch (c->kind) {
            case K_FUN:
                fprintf(stderr, "  call %s, stack %d\n", e->name, sp);
                env = bind_params(&c->free, c->bind, args);
                e = c->bind->body;
                break;
            case K_THUNK:
                stack[sp].is_update = 1;
                stack[sp++].updatee = fn.num;
                fprintf(stderr, "  enter thunk #%ld, push an update frame, stack %d\n", fn.num, sp);
                env = c->free;
                e = c->bind->body;
                break;
            default:
                fprintf(stderr, "  enter #%ld, already a value\n", fn.num);
                ret = fn;
                returning = 1;
            }
        }
        }
    }
}

int main(int argc, char **argv)
{
    struct program *p = NULL;
    if (argc < 2 || (p = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_flags(p);
    stage_fvs(p);
    stage_run(p);
    return 0;
}
