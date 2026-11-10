/*
 * pallene: a cut-down version of Pallene, a statically typed sibling of Lua that is compiled
 * ahead of time to C code which runs inside the real Lua runtime.
 *
 * Pallene values of type integer and boolean are plain C values, but strings, arrays and
 * records are objects of Lua's garbage collector. The collector finds its roots by walking the
 * Lua stack, and C locals are not on it. So Pallene's compiler has to do the bookkeeping
 * itself: it finds, with a live-variable analysis over a CFG of basic blocks, which garbage
 * collected variables are alive at each point where the collector may run (an explicit CheckGC
 * command after every allocation, and every function call), gives variables that are alive at
 * the same time different Lua stack slots, and makes every assignment to such a variable
 * also write the value into its slot. Variables that die before reaching a collection point
 * are never mirrored.
 *
 * This entry skips the lexer and the parser. The program of each example is written by hand as
 * the syntax tree Pallene's parser would hand over, with the type annotations Pallene needs.
 * It keeps the typechecker's cast insertion, the flat three-address IR with structured
 * lowering of `if` and `while`, the GC analysis and the C code generator. Pallene's C output is
 * compiled by a C compiler and linked into Lua. This entry cannot call one, so it executes the
 * same annotated IR on a toy runtime: a heap of strings, a Lua stack, and a collector that
 * runs at every CheckGC and frees whatever the stack does not reach. A wrong GC analysis would
 * free a live string, and the program would crash.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. No machine code is generated (the C text is the output), so code.bin is ignored.
 *
 * Stages:
 *   stage_source     build the syntax tree of the example and print it (stands in for the
 *                    lexer and the parser)
 *   stage_typecheck  type every expression and insert a cast where a value is passed as `any`
 *   stage_to_ir      lower to basic blocks of flat commands, with a CheckGC after allocations
 *   stage_gc         live GC variables at each collection point and the Lua stack slots
 *   stage_coder      render each function as C, with the slot writes and the stack top
 *   stage_run        run the annotated IR on the toy runtime, tracing calls and collections
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 16 /* variables of a function: the live sets are 32-bit masks */
#define MAXB 8
#define MAXC 12
#define STACK 32
#define HEAP 32

enum ty { T_VOID, T_BOOL, T_INT, T_STR, T_ANY };

static const char *const ty_name[] = {"void", "boolean", "integer", "string", "any"};
static const char *const c_type[] = {"void", "int", "lua_Integer", "TString *", "TValue"};

/* Strings and `any` can hold a pointer to a collected object. Integers and booleans cannot. */
static int is_gc(enum ty t)
{
    return t == T_STR || t == T_ANY;
}

/* ------------------------------------------------------------------------------------------
 * The syntax tree
 * ---------------------------------------------------------------------------------------- */

enum xk { X_INT, X_STR, X_VAR, X_BIN, X_CAT, X_CALL, X_CAST };

struct expr {
    enum xk kind;
    long num;
    const char *s; /* a string literal's text, a variable's name, a function's name */
    char op;
    int n;
    struct expr *a[3];
    enum ty ty;
};

enum sk { S_LOCAL, S_ASSIGN, S_IF, S_WHILE, S_RETURN, S_EXPR };

struct stmts;

struct stmt {
    enum sk kind;
    const char *name;
    enum ty ty;
    struct expr *e;
    struct stmts *a, *b;
};

struct stmts {
    struct stmt *s[8];
    int n;
};

struct func {
    const char *name;
    const char *pname;
    enum ty pty;
    int nparams;
    enum ty ret;
    struct stmts *body;
};

struct program {
    struct func f[2];
    int nf;
};

static struct expr epool[64];
static int ne;
static struct stmt spool[40];
static int ns;
static struct stmts lpool[24];
static int nl;

static struct expr *ex(enum xk kind, const char *s, long num, char op, int n, struct expr *a,
                       struct expr *b, struct expr *c)
{
    struct expr *e = &epool[ne++];
    e->kind = kind;
    e->s = s;
    e->num = num;
    e->op = op;
    e->n = n;
    e->a[0] = a;
    e->a[1] = b;
    e->a[2] = c;
    e->ty = T_VOID;
    return e;
}

static struct expr *num(long v)
{
    return ex(X_INT, NULL, v, 0, 0, NULL, NULL, NULL);
}

static struct expr *str(const char *s)
{
    return ex(X_STR, s, 0, 0, 0, NULL, NULL, NULL);
}

static struct expr *var(const char *s)
{
    return ex(X_VAR, s, 0, 0, 0, NULL, NULL, NULL);
}

static struct expr *bin(char op, struct expr *l, struct expr *r)
{
    return ex(X_BIN, NULL, 0, op, 2, l, r, NULL);
}

static struct expr *cat(struct expr *l, struct expr *r)
{
    return ex(X_CAT, NULL, 0, 0, 2, l, r, NULL);
}

static struct expr *call(const char *f, struct expr *arg)
{
    return ex(X_CALL, f, 0, 0, 1, arg, NULL, NULL);
}

static struct stmt *st(enum sk kind, const char *name, enum ty ty, struct expr *e,
                       struct stmts *a, struct stmts *b)
{
    struct stmt *s = &spool[ns++];
    s->kind = kind;
    s->name = name;
    s->ty = ty;
    s->e = e;
    s->a = a;
    s->b = b;
    return s;
}

static struct stmt *local(const char *name, enum ty ty, struct expr *e)
{
    return st(S_LOCAL, name, ty, e, NULL, NULL);
}

static struct stmt *assign(const char *name, struct expr *e)
{
    return st(S_ASSIGN, name, T_VOID, e, NULL, NULL);
}

static struct stmt *ret(struct expr *e)
{
    return st(S_RETURN, NULL, T_VOID, e, NULL, NULL);
}

static struct stmt *expr_stmt(struct expr *e)
{
    return st(S_EXPR, NULL, T_VOID, e, NULL, NULL);
}

static struct stmts *list(int n, ...)
{
    struct stmts *l = &lpool[nl++];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        l->s[l->n++] = va_arg(ap, struct stmt *);
    }
    va_end(ap);
    return l;
}

static struct stmt *if_(struct expr *c, struct stmts *a, struct stmts *b)
{
    return st(S_IF, NULL, T_VOID, c, a, b);
}

static struct stmt *while_(struct expr *c, struct stmts *a)
{
    return st(S_WHILE, NULL, T_VOID, c, a, NULL);
}

/* ------------------------------------------------------------------------------------------
 * Printing the syntax tree
 * ---------------------------------------------------------------------------------------- */

static void pstr(const char *s)
{
    fputc('"', stderr);
    for (; *s; s++) {
        if (*s == '\n') {
            fputs("\\n", stderr);
        } else {
            fputc(*s, stderr);
        }
    }
    fputc('"', stderr);
}

static void print_expr(const struct expr *e);

static void print_operand(const struct expr *e)
{
    if (e->kind == X_BIN) {
        fputc('(', stderr);
        print_expr(e);
        fputc(')', stderr);
    } else {
        print_expr(e);
    }
}

static void print_expr(const struct expr *e)
{
    switch (e->kind) {
    case X_INT:
        fprintf(stderr, "%ld", e->num);
        break;
    case X_STR:
        pstr(e->s);
        break;
    case X_VAR:
        fputs(e->s, stderr);
        break;
    case X_BIN:
        print_operand(e->a[0]);
        fprintf(stderr, " %c ", e->op);
        print_operand(e->a[1]);
        break;
    case X_CAT:
        print_expr(e->a[0]);
        fputs(" .. ", stderr);
        print_expr(e->a[1]);
        break;
    case X_CALL:
        fprintf(stderr, "%s(", e->s);
        print_expr(e->a[0]);
        fputc(')', stderr);
        break;
    default: /* X_CAST */
        fprintf(stderr, "cast<%s>(", ty_name[e->ty]);
        print_expr(e->a[0]);
        fputc(')', stderr);
    }
}

static void print_stmts(const struct stmts *l, int ind)
{
    for (int i = 0; i < l->n; i++) {
        const struct stmt *s = l->s[i];
        fprintf(stderr, "%*s", ind, "");
        switch (s->kind) {
        case S_LOCAL:
            fprintf(stderr, "local %s: %s = ", s->name, ty_name[s->ty]);
            print_expr(s->e);
            fputc('\n', stderr);
            break;
        case S_ASSIGN:
            fprintf(stderr, "%s = ", s->name);
            print_expr(s->e);
            fputc('\n', stderr);
            break;
        case S_IF:
            fputs("if ", stderr);
            print_expr(s->e);
            fputs(" then\n", stderr);
            print_stmts(s->a, ind + 4);
            if (s->b->n > 0) {
                fprintf(stderr, "%*selse\n", ind, "");
                print_stmts(s->b, ind + 4);
            }
            fprintf(stderr, "%*send\n", ind, "");
            break;
        case S_WHILE:
            fputs("while ", stderr);
            print_expr(s->e);
            fputs(" do\n", stderr);
            print_stmts(s->a, ind + 4);
            fprintf(stderr, "%*send\n", ind, "");
            break;
        case S_RETURN:
            fputs("return ", stderr);
            print_expr(s->e);
            fputc('\n', stderr);
            break;
        default: /* S_EXPR */
            print_expr(s->e);
            fputc('\n', stderr);
        }
    }
}

static void print_program(const struct program *p)
{
    for (int i = 0; i < p->nf; i++) {
        const struct func *f = &p->f[i];
        fprintf(stderr, "function %s(", f->name);
        if (f->nparams > 0) {
            fprintf(stderr, "%s: %s", f->pname, ty_name[f->pty]);
        }
        fputc(')', stderr);
        if (f->ret != T_VOID) {
            fprintf(stderr, ": %s", ty_name[f->ret]);
        }
        fputc('\n', stderr);
        print_stmts(f->body, 4);
        fputs("end\n", stderr);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_source: the example programs, as a typed language would write them
 * ---------------------------------------------------------------------------------------- */

static struct program prog;

static struct func *add_func(const char *name, enum ty ret, struct stmts *body)
{
    struct func *f = &prog.f[prog.nf++];
    f->name = name;
    f->ret = ret;
    f->body = body;
    return f;
}

static struct program *stage_source(const char *example)
{
    if (strcmp(example, "hello") == 0) {
        add_func("main", T_VOID,
                 list(6, local("hello", T_STR, str("Hello, ")),
                      local("world", T_STR, str("world!")), local("nl", T_STR, str("\n")),
                      local("tmp", T_STR, cat(var("hello"), var("world"))),
                      local("line", T_STR, cat(var("tmp"), var("nl"))),
                      expr_stmt(call("io.write", var("line")))));
    } else if (strcmp(example, "arithmetic") == 0) {
        add_func("main", T_VOID,
                 list(3,
                      local("n", T_INT,
                            bin('-', bin('*', bin('+', num(2), num(3)), num(4)), num(6))),
                      local("s", T_STR, cat(call("tostring", var("n")), str("\n"))),
                      expr_stmt(call("io.write", var("s")))));
    } else if (strcmp(example, "branch") == 0) {
        add_func("main", T_VOID,
                 list(6, local("a", T_INT, num(3)), local("b", T_INT, num(5)),
                      local("s", T_STR, str("")),
                      if_(bin('<', var("a"), var("b")),
                          list(1, assign("s", call("tostring", var("b")))),
                          list(1, assign("s", call("tostring", var("a"))))),
                      local("line", T_STR, cat(var("s"), str("\n"))),
                      expr_stmt(call("io.write", var("line")))));
    } else if (strcmp(example, "loop") == 0) {
        add_func("main", T_VOID,
                 list(5, local("sum", T_INT, num(0)), local("i", T_INT, num(1)),
                      while_(bin('<', var("i"), num(11)),
                             list(2, assign("sum", bin('+', var("sum"), var("i"))),
                                  assign("i", bin('+', var("i"), num(1))))),
                      local("s", T_STR, cat(call("tostring", var("sum")), str("\n"))),
                      expr_stmt(call("io.write", var("s")))));
    } else if (strcmp(example, "call") == 0) {
        struct func *fact = add_func(
            "fact", T_INT,
            list(2,
                 if_(bin('<', var("n"), num(2)), list(1, ret(num(1))), list(0)),
                 ret(bin('*', var("n"), call("fact", bin('-', var("n"), num(1)))))));
        fact->pname = "n";
        fact->pty = T_INT;
        fact->nparams = 1;
        add_func("main", T_VOID,
                 list(3, local("r", T_INT, call("fact", num(5))),
                      local("s", T_STR, cat(call("tostring", var("r")), str("\n"))),
                      expr_stmt(call("io.write", var("s")))));
    } else {
        return NULL;
    }
    fputs("== stage_source: the program of the example ==\n", stderr);
    print_program(&prog);
    return &prog;
}

/* ------------------------------------------------------------------------------------------
 * stage_typecheck: types, and a cast wherever a value becomes `any`
 * ---------------------------------------------------------------------------------------- */

struct sym {
    const char *name;
    enum ty ty;
};

static struct sym scope[MAXV];
static int nscope;

static enum ty lookup_var(const char *name)
{
    int i = nscope;
    while (strcmp(scope[--i].name, name) != 0) {
    }
    return scope[i].ty;
}

static int fn_index(const struct program *p, const char *name)
{
    int i = p->nf;
    while (strcmp(p->f[--i].name, name) != 0) {
    }
    return i;
}

static enum ty synth(const struct program *p, struct expr *e);

/* Synthesize the type of *slot and, when it is not the type that is wanted, wrap it in a
 * cast. The only conversion the examples need is to `any`, so a cast is always a ToDyn. */
static void verify(const struct program *p, struct expr **slot, enum ty want)
{
    if (synth(p, *slot) != want) {
        struct expr *c = ex(X_CAST, NULL, 0, 0, 1, *slot, NULL, NULL);
        c->ty = want;
        *slot = c;
    }
}

static enum ty synth(const struct program *p, struct expr *e)
{
    switch (e->kind) {
    case X_INT:
        e->ty = T_INT;
        break;
    case X_STR:
        e->ty = T_STR;
        break;
    case X_VAR:
        e->ty = lookup_var(e->s);
        break;
    case X_BIN:
        verify(p, &e->a[0], T_INT);
        verify(p, &e->a[1], T_INT);
        e->ty = e->op == '<' ? T_BOOL : T_INT;
        break;
    case X_CAT:
        verify(p, &e->a[0], T_STR);
        verify(p, &e->a[1], T_STR);
        e->ty = T_STR;
        break;
    default: { /* X_CALL (X_CAST is only made by verify, which sets its type) */
        enum ty param = T_STR;
        enum ty result = T_VOID;
        if (strcmp(e->s, "tostring") == 0) {
            param = T_ANY;
            result = T_STR;
        } else if (strcmp(e->s, "io.write") != 0) {
            const struct func *f = &p->f[fn_index(p, e->s)];
            param = f->pty;
            result = f->ret;
        }
        verify(p, &e->a[0], param);
        e->ty = result;
    }
    }
    return e->ty;
}

static void check_stmts(const struct program *p, const struct func *f, struct stmts *l)
{
    for (int i = 0; i < l->n; i++) {
        struct stmt *s = l->s[i];
        switch (s->kind) {
        case S_LOCAL:
            verify(p, &s->e, s->ty);
            scope[nscope].name = s->name;
            scope[nscope++].ty = s->ty;
            break;
        case S_ASSIGN:
            verify(p, &s->e, lookup_var(s->name));
            break;
        case S_IF:
            synth(p, s->e);
            check_stmts(p, f, s->a);
            check_stmts(p, f, s->b);
            break;
        case S_WHILE:
            synth(p, s->e);
            check_stmts(p, f, s->a);
            break;
        case S_RETURN:
            verify(p, &s->e, f->ret);
            break;
        default: /* S_EXPR */
            synth(p, s->e);
        }
    }
}

static void stage_typecheck(struct program *p)
{
    for (int i = 0; i < p->nf; i++) {
        nscope = 0;
        if (p->f[i].nparams > 0) {
            scope[nscope].name = p->f[i].pname;
            scope[nscope++].ty = p->f[i].pty;
        }
        check_stmts(p, &p->f[i], p->f[i].body);
    }
    fputs("== stage_typecheck: the program with its casts ==\n", stderr);
    print_program(p);
}

/* ------------------------------------------------------------------------------------------
 * The IR: a list of basic blocks per function, each a list of flat commands
 * ---------------------------------------------------------------------------------------- */

enum vk { V_INT, V_STR, V_VAR };

struct val {
    enum vk kind;
    long num;
    const char *s;
    int var;
};

enum ck { C_MOVE, C_BIN, C_CAT, C_TODYN, C_CALL, C_TOSTR, C_WRITE, C_JMP, C_JMPIF, C_CHECKGC };

struct cmd {
    enum ck kind;
    int dst; /* a variable id, or -1 */
    struct val src[2];
    int n; /* how many of src are used */
    char op;
    int t[2]; /* jump targets */
    const char *callee;
    int fn;
    enum ty srcty;
    unsigned live; /* GC variables alive before this command (set at collection points) */
    int mirror;    /* Lua stack slot this command's result is copied to, or -1 */
    int top;       /* stack top, in slots above the frame base, at a collection point */
};

struct blk {
    struct cmd c[MAXC];
    int n;
    int jumped;
};

struct ifunc {
    const char *name;
    int nparams;
    int ret_var;
    const char *vname[MAXV];
    enum ty vty[MAXV];
    int nvars;
    struct blk b[MAXB];
    int nb;
    int rets[4];
    int nrets;
    int slot[MAXV];
    int frame;
};

struct irmod {
    struct ifunc f[2];
    int nf;
};

static struct irmod mod;
static struct ifunc *cur;
static const struct program *cur_prog;

static int add_local(const char *name, enum ty t)
{
    cur->vname[cur->nvars] = name;
    cur->vty[cur->nvars] = t;
    return cur->nvars++;
}

static int find_var(const char *name)
{
    int i = 0;
    while (cur->vname[i] == NULL || strcmp(cur->vname[i], name) != 0) {
        i++;
    }
    return i;
}

static struct cmd *emit(enum ck kind, int dst, int n)
{
    struct blk *b = &cur->b[cur->nb - 1];
    struct cmd *c = &b->c[b->n++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->dst = dst;
    c->n = n;
    c->t[0] = c->t[1] = -1;
    c->mirror = -1;
    b->jumped = kind == C_JMP || kind == C_JMPIF;
    return c;
}

/* Close the current block with a jump to the next one, unless it already ends in a jump, and
 * start the next block. Returns the new block's number. */
static int finish_block(void)
{
    int next = cur->nb;
    if (!cur->b[next - 1].jumped) {
        emit(C_JMP, -1, 0)->t[0] = next;
    }
    cur->nb++;
    return next;
}

static void exp_to_assignment(int dst, const struct expr *e);

static struct val exp_to_value(const struct expr *e)
{
    struct val v;
    memset(&v, 0, sizeof v);
    switch (e->kind) {
    case X_INT:
        v.kind = V_INT;
        v.num = e->num;
        break;
    case X_STR:
        v.kind = V_STR;
        v.s = e->s;
        break;
    case X_VAR:
        v.kind = V_VAR;
        v.var = find_var(e->s);
        break;
    default: /* anything else goes through a temporary */
        v.kind = V_VAR;
        v.var = add_local(NULL, e->ty);
        exp_to_assignment(v.var, e);
    }
    return v;
}

static void exp_to_assignment(int dst, const struct expr *e)
{
    struct cmd *c;
    switch (e->kind) {
    case X_BIN: {
        struct val a = exp_to_value(e->a[0]);
        struct val b = exp_to_value(e->a[1]);
        c = emit(C_BIN, dst, 2);
        c->op = e->op;
        c->src[0] = a;
        c->src[1] = b;
        break;
    }
    case X_CAT: {
        struct val a = exp_to_value(e->a[0]);
        struct val b = exp_to_value(e->a[1]);
        c = emit(C_CAT, dst, 2);
        c->src[0] = a;
        c->src[1] = b;
        emit(C_CHECKGC, -1, 0); /* a concatenation allocates */
        break;
    }
    case X_CALL: {
        struct val a = exp_to_value(e->a[0]);
        if (strcmp(e->s, "tostring") == 0) {
            c = emit(C_TOSTR, dst, 1);
        } else if (strcmp(e->s, "io.write") == 0) {
            c = emit(C_WRITE, -1, 1);
        } else {
            c = emit(C_CALL, dst, 1);
            c->callee = e->s;
            c->fn = fn_index(cur_prog, e->s);
        }
        c->src[0] = a;
        break;
    }
    case X_CAST: {
        struct val a = exp_to_value(e->a[0]);
        c = emit(C_TODYN, dst, 1);
        c->srcty = e->a[0]->ty;
        c->src[0] = a;
        break;
    }
    default: { /* a constant or a variable */
        struct val a = exp_to_value(e);
        c = emit(C_MOVE, dst, 1);
        c->src[0] = a;
    }
    }
}

static void convert_stmts(const struct stmts *l)
{
    for (int i = 0; i < l->n; i++) {
        const struct stmt *s = l->s[i];
        switch (s->kind) {
        case S_LOCAL:
            exp_to_assignment(add_local(s->name, s->ty), s->e);
            break;
        case S_ASSIGN:
            exp_to_assignment(find_var(s->name), s->e);
            break;
        case S_IF: {
            struct val cond = exp_to_value(s->e);
            struct cmd *jif = emit(C_JMPIF, -1, 1);
            jif->src[0] = cond;
            int then_b = finish_block();
            convert_stmts(s->a);
            struct cmd *then_end = emit(C_JMP, -1, 0);
            int else_b = finish_block();
            convert_stmts(s->b);
            if (cur->b[cur->nb - 1].n != 0) {
                finish_block();
            }
            jif->t[0] = then_b;
            jif->t[1] = else_b;
            then_end->t[0] = cur->nb - 1;
            break;
        }
        case S_WHILE: {
            int begin = finish_block();
            struct val cond = exp_to_value(s->e);
            struct cmd *jif = emit(C_JMPIF, -1, 1);
            jif->src[0] = cond;
            int body = finish_block();
            convert_stmts(s->a);
            emit(C_JMP, -1, 0)->t[0] = begin;
            int after = finish_block();
            jif->t[0] = body;
            jif->t[1] = after;
            break;
        }
        case S_RETURN: {
            struct val v = exp_to_value(s->e);
            struct cmd *c = emit(C_MOVE, cur->ret_var, 1);
            c->src[0] = v;
            cur->rets[cur->nrets++] = cur->nb - 1;
            finish_block();
            break;
        }
        default: /* S_EXPR, a call whose result is dropped */
            exp_to_assignment(-1, s->e);
        }
    }
}

static void convert_func(const struct func *f, struct ifunc *out)
{
    cur = out;
    out->name = f->name;
    out->nparams = f->nparams;
    out->ret_var = -1;
    if (f->nparams > 0) {
        add_local(f->pname, f->pty);
    }
    if (f->ret != T_VOID) {
        out->ret_var = add_local(NULL, f->ret);
    }
    out->nb = 1;
    convert_stmts(f->body);
    /* Every return jumps to the last block, the exit. */
    for (int i = 0; i < out->nrets; i++) {
        struct blk *b = &out->b[out->rets[i]];
        b->c[b->n - 1].t[0] = out->nb - 1;
    }
}

static void pvar(const struct ifunc *f, int v, int cstyle)
{
    if (cstyle) {
        fprintf(stderr, "x%d", v + 1);
    } else if (f->vname[v] != NULL) {
        fputs(f->vname[v], stderr);
    } else {
        fprintf(stderr, "$t%d", v);
    }
}

static void pval(const struct ifunc *f, struct val x, int cstyle)
{
    if (x.kind == V_INT) {
        fprintf(stderr, "%ld", x.num);
    } else if (x.kind == V_STR) {
        pstr(x.s);
    } else {
        pvar(f, x.var, cstyle);
    }
}

static void print_cmd(const struct ifunc *f, const struct cmd *c)
{
    if (c->kind == C_JMP) {
        fprintf(stderr, "goto B%d", c->t[0]);
    } else if (c->kind == C_JMPIF) {
        fputs("if ", stderr);
        pval(f, c->src[0], 0);
        fprintf(stderr, " goto B%d else B%d", c->t[0], c->t[1]);
    } else if (c->kind == C_CHECKGC) {
        fputs("CheckGC", stderr);
    } else {
        if (c->dst >= 0) {
            pvar(f, c->dst, 0);
            fputs(" = ", stderr);
        }
        switch (c->kind) {
        case C_MOVE:
            pval(f, c->src[0], 0);
            break;
        case C_BIN:
            pval(f, c->src[0], 0);
            fprintf(stderr, " %c ", c->op);
            pval(f, c->src[1], 0);
            break;
        case C_CAT:
            pval(f, c->src[0], 0);
            fputs(" .. ", stderr);
            pval(f, c->src[1], 0);
            break;
        case C_TODYN:
            fprintf(stderr, "ToDyn<%s>(", ty_name[c->srcty]);
            pval(f, c->src[0], 0);
            fputc(')', stderr);
            break;
        case C_CALL:
            fprintf(stderr, "CallStatic %s(", c->callee);
            pval(f, c->src[0], 0);
            fputc(')', stderr);
            break;
        case C_TOSTR:
            fputs("BuiltinTostring(", stderr);
            pval(f, c->src[0], 0);
            fputc(')', stderr);
            break;
        default: /* C_WRITE */
            fputs("BuiltinIoWrite(", stderr);
            pval(f, c->src[0], 0);
            fputc(')', stderr);
        }
    }
    fputc('\n', stderr);
}

static void stage_to_ir(const struct program *p)
{
    cur_prog = p;
    mod.nf = p->nf;
    fputs("== stage_to_ir: basic blocks of flat commands ==\n", stderr);
    for (int i = 0; i < p->nf; i++) {
        struct ifunc *f = &mod.f[i];
        convert_func(&p->f[i], f);
        fprintf(stderr, "function %s\n  variables:", f->name);
        for (int v = 0; v < f->nvars; v++) {
            fputc(' ', stderr);
            pvar(f, v, 0);
            fprintf(stderr, ":%s", ty_name[f->vty[v]]);
        }
        fputc('\n', stderr);
        for (int b = 0; b < f->nb; b++) {
            fprintf(stderr, "  B%d:\n", b);
            for (int k = 0; k < f->b[b].n; k++) {
                fputs("    ", stderr);
                print_cmd(f, &f->b[b].c[k]);
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_gc: which GC variables are alive where the collector may run, and where they live on
 * the Lua stack
 * ---------------------------------------------------------------------------------------- */

/* The collector may run at a CheckGC, and inside any call. */
static int is_gc_point(const struct cmd *c)
{
    return c->kind == C_CALL || c->kind == C_CHECKGC;
}

static unsigned gc_bit(const struct ifunc *f, int v)
{
    return is_gc(f->vty[v]) ? 1u << v : 0u;
}

/* The variables alive before a command, from those alive after it. Only GC variables count. */
static unsigned step(const struct ifunc *f, const struct cmd *c, unsigned live)
{
    if (c->dst >= 0) {
        live &= ~gc_bit(f, c->dst);
    }
    for (int i = 0; i < c->n; i++) {
        if (c->src[i].kind == V_VAR) {
            live |= gc_bit(f, c->src[i].var);
        }
    }
    return live;
}

static int succs(const struct ifunc *f, int b, int out[2])
{
    const struct blk *bl = &f->b[b];
    if (bl->jumped) {
        const struct cmd *last = &bl->c[bl->n - 1];
        out[0] = last->t[0];
        out[1] = last->t[1];
        return last->kind == C_JMP ? 1 : 2;
    }
    return 0;
}

static void postorder(int pred[MAXB][MAXB], int npred[MAXB], int b, int seen[MAXB],
                      int order[MAXB], int *n)
{
    seen[b] = 1;
    for (int i = 0; i < npred[b]; i++) {
        if (!seen[pred[b][i]]) {
            postorder(pred, npred, pred[b][i], seen, order, n);
        }
    }
    order[(*n)++] = b;
}

static void analyze(struct ifunc *f)
{
    int pred[MAXB][MAXB];
    int npred[MAXB] = {0};
    for (int b = 0; b < f->nb; b++) {
        int s[2];
        int ns_ = succs(f, b, s);
        for (int i = 0; i < ns_; i++) {
            pred[s[i]][npred[s[i]]++] = b;
        }
    }

    /* Walk the blocks backwards from the exit, in reverse postorder of the reversed graph, until
     * no live set changes. Unreachable blocks are not visited. */
    int exit_b = f->nb - 1;
    int seen[MAXB] = {0};
    int order[MAXB];
    int n = 0;
    postorder(pred, npred, exit_b, seen, order, &n);
    unsigned init = f->ret_var >= 0 ? gc_bit(f, f->ret_var) : 0u;
    unsigned in[MAXB] = {0};
    unsigned out[MAXB] = {0};
    for (int changed = 1; changed;) {
        changed = 0;
        for (int k = n - 1; k >= 0; k--) {
            int b = order[k];
            int s[2];
            int ns_ = succs(f, b, s);
            unsigned o = b == exit_b ? init : 0u;
            for (int i = 0; i < ns_; i++) {
                o |= in[s[i]];
            }
            out[b] = o;
            unsigned live = o;
            for (int j = f->b[b].n - 1; j >= 0; j--) {
                live = step(f, &f->b[b].c[j], live);
            }
            if (live != in[b]) {
                in[b] = live;
                changed = 1;
            }
        }
    }

    /* The live set before each collection point, and which variables share one. */
    unsigned together[MAXV] = {0};
    fprintf(stderr, "function %s\n", f->name);
    for (int b = 0; b < f->nb; b++) {
        unsigned live = out[b];
        for (int j = f->b[b].n - 1; j >= 0; j--) {
            struct cmd *c = &f->b[b].c[j];
            live = step(f, c, live);
            if (is_gc_point(c)) {
                c->live = live;
                for (int v = 0; v < f->nvars; v++) {
                    if (live >> v & 1u) {
                        together[v] |= live;
                    }
                }
            }
        }
    }
    for (int b = 0; b < f->nb; b++) {
        for (int j = 0; j < f->b[b].n; j++) {
            const struct cmd *c = &f->b[b].c[j];
            if (is_gc_point(c)) {
                fprintf(stderr, "  B%d command %d, %s: live {", b, j,
                        c->kind == C_CALL ? "CallStatic" : "CheckGC");
                const char *sep = "";
                for (int v = 0; v < f->nvars; v++) {
                    if (c->live >> v & 1u) {
                        fputs(sep, stderr);
                        pvar(f, v, 0);
                        sep = ", ";
                    }
                }
                fputs("}\n", stderr);
            }
        }
    }

    /* Greedy coloring: a variable gets the lowest slot that none of the variables it is alive
     * with has taken. A variable that is never alive at a collection point gets no slot. */
    f->frame = 0;
    for (int v = 0; v < MAXV; v++) {
        f->slot[v] = -1;
    }
    fputs("  stack slots:", stderr);
    for (int v = 0; v < f->nvars; v++) {
        if (together[v] != 0) {
            unsigned taken = 0;
            for (int u = 0; u < f->nvars; u++) {
                if (u != v && (together[v] >> u & 1u) && f->slot[u] >= 0) {
                    taken |= 1u << f->slot[u];
                }
            }
            int s = 0;
            while (taken >> s & 1u) {
                s++;
            }
            f->slot[v] = s;
            f->frame = s + 1 > f->frame ? s + 1 : f->frame;
            fputc(' ', stderr);
            pvar(f, v, 0);
            fprintf(stderr, "=%d", s);
        }
    }
    fprintf(stderr, "\n  frame size: %d\n", f->frame);
}

static void stage_gc(void)
{
    fputs("== stage_gc: live GC variables and Lua stack slots ==\n", stderr);
    for (int i = 0; i < mod.nf; i++) {
        analyze(&mod.f[i]);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_coder: C text, with the stack writes and the stack top
 * ---------------------------------------------------------------------------------------- */

static const char *const setter[] = {"", "setbvalue", "setivalue", "setsvalue", ""};

static void render_cmd(const struct ifunc *f, const struct cmd *c)
{
    fputs("    ", stderr);
    switch (c->kind) {
    case C_MOVE:
    case C_BIN:
    case C_CAT:
    case C_TOSTR:
    case C_CALL:
        pvar(f, c->dst, 1);
        fputs(" = ", stderr);
        break;
    default:
        break;
    }
    switch (c->kind) {
    case C_MOVE:
        pval(f, c->src[0], 1);
        fputc(';', stderr);
        break;
    case C_BIN:
        pval(f, c->src[0], 1);
        fprintf(stderr, " %c ", c->op);
        pval(f, c->src[1], 1);
        fputc(';', stderr);
        break;
    case C_CAT:
        fputs("pallene_string_concatN(L, 2, (TString *[]){", stderr);
        pval(f, c->src[0], 1);
        fputs(", ", stderr);
        pval(f, c->src[1], 1);
        fputs("});", stderr);
        break;
    case C_TODYN:
        fprintf(stderr, "%s(&", setter[c->srcty]);
        pvar(f, c->dst, 1);
        fputs(", ", stderr);
        pval(f, c->src[0], 1);
        fputs(");", stderr);
        break;
    case C_CALL:
        fprintf(stderr, "L->top.p = base + %d; function_%02d(L, ", c->top, c->fn + 1);
        pval(f, c->src[0], 1);
        fputs(");", stderr);
        break;
    case C_TOSTR:
        fputs("pallene_tostring(L, ", stderr);
        pval(f, c->src[0], 1);
        fputs(");", stderr);
        break;
    case C_WRITE:
        fputs("pallene_io_write(L, ", stderr);
        pval(f, c->src[0], 1);
        fputs(");", stderr);
        break;
    case C_JMP:
        fprintf(stderr, "goto L%d;", c->t[0]);
        break;
    case C_JMPIF:
        fputs("if (", stderr);
        pval(f, c->src[0], 1);
        fprintf(stderr, ") { goto L%d; } else { goto L%d; }", c->t[0], c->t[1]);
        break;
    default: /* C_CHECKGC */
        fprintf(stderr, "L->top.p = base + %d; luaC_condGC(L);", c->top);
    }
    if (c->mirror >= 0) {
        fprintf(stderr, " s2v(base + %d) = ", c->mirror);
        pvar(f, c->dst, 1);
        fputc(';', stderr);
    }
    fputc('\n', stderr);
}

static void stage_coder(void)
{
    fputs("== stage_coder: C code ==\n", stderr);
    for (int i = 0; i < mod.nf; i++) {
        struct ifunc *f = &mod.f[i];
        /* Every assignment to a variable with a slot is followed by a write to the slot, and
         * the stack top at a collection point is just above the highest live slot. */
        for (int b = 0; b < f->nb; b++) {
            for (int j = 0; j < f->b[b].n; j++) {
                struct cmd *c = &f->b[b].c[j];
                if (c->dst >= 0) {
                    c->mirror = f->slot[c->dst];
                }
                unsigned used = 0; /* the slots of the variables alive here */
                for (int v = 0; v < f->nvars; v++) {
                    if (c->live >> v & 1u) {
                        used |= 1u << f->slot[v];
                    }
                }
                while (used >> c->top) {
                    c->top++;
                }
            }
        }
        enum ty rt = f->ret_var >= 0 ? f->vty[f->ret_var] : T_VOID;
        fprintf(stderr, "/* %s */\nstatic %s function_%02d(lua_State *L", f->name, c_type[rt], i + 1);
        if (f->nparams > 0) {
            fprintf(stderr, ", %s x1", c_type[f->vty[0]]);
        }
        fputs(") {\n", stderr);
        if (f->frame > 0) {
            fprintf(stderr, "    if (!lua_checkstack(L, %d)) { /* error */ }\n", f->frame);
        }
        fputs("    StackValue *base = L->top.p;\n", stderr);
        for (int v = f->nparams; v < f->nvars; v++) {
            fprintf(stderr, "    %s ", c_type[f->vty[v]]);
            pvar(f, v, 1);
            fputs(f->vty[v] == T_ANY ? " = {{0},0}; /* " : "; /* ", stderr);
            pvar(f, v, 0);
            fputs(" */\n", stderr);
        }
        for (int b = 0; b < f->nb; b++) {
            fprintf(stderr, "  L%d:\n", b);
            for (int j = 0; j < f->b[b].n; j++) {
                const struct cmd *c = &f->b[b].c[j];
                /* a jump to the next block is left out */
                if (c->kind != C_JMP || c->t[0] != b + 1) {
                    render_cmd(f, c);
                }
            }
        }
        if (f->ret_var >= 0) {
            fputs("    return ", stderr);
            pvar(f, f->ret_var, 1);
            fputs(";\n", stderr);
        }
        fputs("}\n", stderr);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_run: the annotated IR on a toy runtime
 * ---------------------------------------------------------------------------------------- */

struct obj {
    char *s;
    int mark;
};

static struct obj heap[HEAP];
static int nheap;
static int young[HEAP]; /* the collectable objects that are still allocated */
static int nyoung;
/* The Lua stack: the only roots the collector knows. Values are untagged: an integer, or the
 * number of a heap object. Only variables of a collected type get a slot, and in the examples
 * those are all strings, so every slot below the stack top holds a heap object. */
static long lstack[STACK];

static long new_str(const char *text, int permanent)
{
    int id = nheap++;
    size_t len = strlen(text) + 1;
    heap[id].s = malloc(len);
    memcpy(heap[id].s, text, len);
    heap[id].mark = 0;
    if (!permanent) {
        young[nyoung++] = id;
    }
    return id;
}

/* A full collection, as luaC_condGC does when the collector is in stress mode: mark what the
 * stack below its top reaches, free the rest, and clear the dead part of the stack. */
static void collect(int top)
{
    fprintf(stderr, "  collect: top %d, roots", top);
    for (int i = 0; i < top; i++) {
        heap[lstack[i]].mark = 1;
        fprintf(stderr, " #%ld", lstack[i]);
    }
    fputs(", freed", stderr);
    int keep = 0;
    for (int i = 0; i < nyoung; i++) {
        int id = young[i];
        if (heap[id].mark) {
            heap[id].mark = 0;
            young[keep++] = id;
        } else {
            free(heap[id].s);
            heap[id].s = NULL;
            fprintf(stderr, " #%d", id);
        }
    }
    nyoung = keep;
    fputc('\n', stderr);
    for (int i = top; i < STACK; i++) {
        lstack[i] = 0;
    }
}

static long get(const long *v, struct val x)
{
    if (x.kind == V_INT) {
        return x.num;
    }
    if (x.kind == V_STR) {
        return new_str(x.s, 1); /* literals are not interned */
    }
    return v[x.var];
}

static long run_func(const struct irmod *m, int fi, int base, long arg)
{
    const struct ifunc *f = &m->f[fi];
    long v[MAXV];
    memset(v, 0, sizeof v);
    if (f->nparams > 0) {
        v[0] = arg;
    }
    int b = 0;
    int k = 0;
    while (k < f->b[b].n) {
        const struct cmd *c = &f->b[b].c[k++];
        long r = 0;
        switch (c->kind) {
        case C_MOVE:
        case C_TODYN:
            r = get(v, c->src[0]);
            break;
        case C_BIN: {
            long x = get(v, c->src[0]);
            long y = get(v, c->src[1]);
            r = c->op == '+' ? x + y : c->op == '-' ? x - y : c->op == '*' ? x * y : x < y;
            break;
        }
        case C_CAT: {
            char buf[64];
            snprintf(buf, sizeof buf, "%s%s", heap[get(v, c->src[0])].s, heap[get(v, c->src[1])].s);
            r = new_str(buf, 0);
            break;
        }
        case C_TOSTR: {
            char buf[24];
            snprintf(buf, sizeof buf, "%ld", get(v, c->src[0]));
            r = new_str(buf, 0);
            break;
        }
        case C_CALL:
            fprintf(stderr, "  call %s, base %d\n", c->callee, base + c->top);
            r = run_func(m, c->fn, base + c->top, get(v, c->src[0]));
            break;
        case C_WRITE:
            fputs(heap[get(v, c->src[0])].s, stdout);
            break;
        case C_JMP:
            b = c->t[0];
            k = 0;
            break;
        case C_JMPIF:
            b = get(v, c->src[0]) ? c->t[0] : c->t[1];
            k = 0;
            break;
        default: /* C_CHECKGC */
            collect(base + c->top);
        }
        if (c->dst >= 0) {
            v[c->dst] = r;
        }
        if (c->mirror >= 0) {
            lstack[base + c->mirror] = r;
        }
    }
    return f->ret_var >= 0 ? v[f->ret_var] : 0;
}

static void stage_run(void)
{
    fputs("== stage_run: the program on the toy runtime ==\n", stderr);
    run_func(&mod, mod.nf - 1, 0, 0);
    for (int i = 0; i < nheap; i++) {
        free(heap[i].s);
    }
}

int main(int argc, char **argv)
{
    struct program *p = NULL;
    if (argc < 2 || (p = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_typecheck(p);
    stage_to_ir(p);
    stage_gc();
    stage_coder();
    stage_run();
    return 0;
}
