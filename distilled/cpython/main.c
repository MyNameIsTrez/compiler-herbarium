/*
 * cpython: a cut-down version of CPython's bytecode compiler and eval loop.
 *
 * CPython compiles a module in a fixed sequence. The AST goes through the symbol table, which
 * decides what kind of name every name is. The code generator walks the tree and emits a flat
 * sequence of instructions with labels. That sequence is split into basic blocks, a CFG
 * optimizer rewrites the blocks, and the assembler picks the final opcodes, computes the jump
 * offsets and writes the bytes of a code object, caches included. A function is compiled the
 * same way, as a nested code unit, before the code that makes it. The eval loop then runs the
 * code object on a value stack, one frame per call.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the form after each stage on stderr. The code.bin
 *   argument is accepted and ignored: the result is bytecode for an interpreter, not machine code.
 *
 * Stages, in the order of the real pipeline:
 *   stage_ast            the AST that Python's parser would build
 *   stage_symtable       symtable.c: the scope of each name
 *   stage_codegen        codegen.c: an instruction sequence with labels, per code unit
 *   stage_cfg            flowgraph.c: the sequence split into basic blocks
 *   stage_optimize_cfg   flowgraph.c optimize_cfg and its helpers
 *   stage_assemble       flowgraph.c and assemble.c: stack depth, jumps, borrowed loads, bytes
 *   stage_run            ceval.c: the eval loop on the code objects
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

#define MAX_NODES 40
#define MAX_SCOPES 3
#define MAX_SYMS 4
#define MAX_UNITS 3
#define MAX_SEQ 48
#define MAX_LABELS 8
#define MAX_BLOCKS 10
#define MAX_BI 16 /* instructions in one block */
#define MAX_NAMES 4
#define MAX_CONSTS 12
#define MAX_CODE 160
#define MAX_STACK 16
#define MAX_COPY_SIZE 4 /* flowgraph.c: the longest exit block that is copied instead of jumped to */

/* Only the shapes that the examples need are handled. Anything else stops the program here. */
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
 * Opcodes. The numbers are the real ones of the 3.16 development branch, so that the bytes in
 * the dumps are the bytes of a real code object. JUMP and JUMP_NO_INTERRUPT are pseudo
 * instructions, which exist only until the assembler picks a direction. LABEL is not an
 * instruction: it marks a position in the sequence that codegen.c builds.
 * ---------------------------------------------------------------------------------------- */

enum {
    O_NOP, O_RESUME, O_LOAD_CONST, O_LOAD_SMALL_INT, O_LOAD_COMMON_CONSTANT, O_LOAD_FAST,
    O_LOAD_FAST_BORROW, O_LOAD_GLOBAL, O_LOAD_NAME, O_STORE_NAME, O_PUSH_NULL, O_POP_TOP,
    O_TO_BOOL, O_COMPARE_OP, O_BINARY_OP, O_CALL, O_MAKE_FUNCTION, O_POP_JUMP_IF_FALSE,
    O_NOT_TAKEN, O_RETURN_VALUE, O_JUMP, O_JUMP_NO_INTERRUPT, O_JUMP_BACKWARD, O_LABEL,
};

struct opinfo {
    const char *name;
    int code;    /* the real opcode, or -1 for a pseudo instruction */
    int caches;  /* inline cache entries that follow the instruction */
    int hasarg;  /* whether the dumps show the argument */
    int pop, push;
    int isjump;
};

static const struct opinfo ops[] = {
    [O_NOP] = {"NOP", 25, 0, 0, 0, 0, 0},
    [O_RESUME] = {"RESUME", 128, 1, 1, 0, 0, 0},
    [O_LOAD_CONST] = {"LOAD_CONST", 78, 0, 1, 0, 1, 0},
    [O_LOAD_SMALL_INT] = {"LOAD_SMALL_INT", 90, 0, 1, 0, 1, 0},
    [O_LOAD_COMMON_CONSTANT] = {"LOAD_COMMON_CONSTANT", 77, 0, 1, 0, 1, 0},
    [O_LOAD_FAST] = {"LOAD_FAST", 80, 0, 1, 0, 1, 0},
    [O_LOAD_FAST_BORROW] = {"LOAD_FAST_BORROW", 82, 0, 1, 0, 1, 0},
    [O_LOAD_GLOBAL] = {"LOAD_GLOBAL", 88, 4, 1, 0, 1, 0}, /* pushes one more with the NULL bit */
    [O_LOAD_NAME] = {"LOAD_NAME", 89, 0, 1, 0, 1, 0},
    [O_STORE_NAME] = {"STORE_NAME", 112, 0, 1, 1, 0, 0},
    [O_PUSH_NULL] = {"PUSH_NULL", 31, 0, 0, 0, 1, 0},
    [O_POP_TOP] = {"POP_TOP", 29, 0, 0, 1, 0, 0},
    [O_TO_BOOL] = {"TO_BOOL", 37, 3, 0, 1, 1, 0},
    [O_COMPARE_OP] = {"COMPARE_OP", 54, 1, 1, 2, 1, 0},
    [O_BINARY_OP] = {"BINARY_OP", 42, 5, 1, 2, 1, 0},
    [O_CALL] = {"CALL", 50, 3, 1, 0, 1, 0}, /* pops the callable, the NULL and the arguments */
    [O_MAKE_FUNCTION] = {"MAKE_FUNCTION", 21, 0, 0, 1, 1, 0},
    [O_POP_JUMP_IF_FALSE] = {"POP_JUMP_IF_FALSE", 96, 1, 1, 1, 0, 1},
    [O_NOT_TAKEN] = {"NOT_TAKEN", 26, 0, 0, 0, 0, 0},
    [O_RETURN_VALUE] = {"RETURN_VALUE", 33, 0, 0, 1, 0, 0},
    [O_JUMP] = {"JUMP", -1, 0, 1, 0, 0, 1},
    [O_JUMP_NO_INTERRUPT] = {"JUMP_NO_INTERRUPT", -1, 0, 1, 0, 0, 1},
    [O_JUMP_BACKWARD] = {"JUMP_BACKWARD", 71, 1, 1, 0, 0, 1},
    [O_LABEL] = {"LABEL", -1, 0, 0, 0, 0, 0},
};

#define NB_ADD 0
#define NB_MULTIPLY 5
#define NB_SUBTRACT 10
static const char *const nb_names[11] = {[NB_ADD] = "Add", [NB_MULTIPLY] = "Mult", [NB_SUBTRACT] = "Sub"};
static const char *const nb_syms[11] = {[NB_ADD] = "+", [NB_MULTIPLY] = "*", [NB_SUBTRACT] = "-"};

#define CMP_LT 2           /* the mask bit of < in a COMPARE_OP argument */
#define CMP_TO_BOOL 16     /* set when the TO_BOOL that followed has been folded in */
#define CONSTANT_NONE 7    /* LOAD_COMMON_CONSTANT's argument for None */

struct ins {
    int op, arg;
    struct block *target; /* in a block: where a jump goes. In the sequence: arg is a label */
    int offset;           /* in code units, set by the assembler */
};

struct block {
    int id, n;
    struct ins in[MAX_BI];
    struct block *next;
    int visited, startdepth, pos;
    int offset;
};

static int pops(const struct ins *x) { return x->op == O_CALL ? x->arg + 2 : ops[x->op].pop; }
static int pushes(const struct ins *x)
{
    return x->op == O_LOAD_GLOBAL ? 1 + (x->arg & 1) : ops[x->op].push;
}

/* Where a block stops: a jump, a conditional jump or a return. */
static int is_block_end(int op) { return ops[op].isjump | (op == O_RETURN_VALUE); }
/* Where control never continues into the next block. */
static int is_uncond(int op) { return is_block_end(op) & (op != O_POP_JUMP_IF_FALSE); }
static int has_fallthrough(const struct block *b) { return !is_uncond(b->in[b->n - 1].op); }

/* ------------------------------------------------------------------------------------------
 * stage_ast
 *
 * The parser's output, written by hand as the calls the parser would make. Only the nodes that
 * the examples use exist, and a call has one argument. The Lua examples map onto Python
 * programs: `local` disappears, and `local function` becomes `def`, so that the names are
 * module globals, which is how Python scopes them.
 * ---------------------------------------------------------------------------------------- */

enum { N_INT, N_STR, N_NAME, N_BINOP, N_COMPARE, N_CALL, N_ASSIGN, N_EXPR, N_IF, N_WHILE,
       N_RETURN, N_DEF };

struct node {
    int kind;
    const char *s; /* a name or a string */
    int64_t v;     /* a number, or the BINARY_OP argument of a binop, or a scope index of a def */
    struct node *a, *b, *c, *next;
};

static struct node nodes[MAX_NODES];
static int nnodes;

static struct node *mk(int kind, const char *s, int64_t v, struct node *a, struct node *b,
                       struct node *c)
{
    require(nnodes < MAX_NODES, "more nodes");
    struct node *n = &nodes[nnodes++];
    n->kind = kind;
    n->s = s;
    n->v = v;
    n->a = a;
    n->b = b;
    n->c = c;
    return n;
}

static struct node *num(int64_t v) { return mk(N_INT, NULL, v, NULL, NULL, NULL); }
static struct node *str(const char *s) { return mk(N_STR, s, 0, NULL, NULL, NULL); }
static struct node *name(const char *s) { return mk(N_NAME, s, 0, NULL, NULL, NULL); }
static struct node *binop(int nb, struct node *l, struct node *r) { return mk(N_BINOP, NULL, nb, l, r, NULL); }
static struct node *lt(struct node *l, struct node *r) { return mk(N_COMPARE, NULL, 0, l, r, NULL); }
static struct node *call(struct node *f, struct node *arg) { return mk(N_CALL, NULL, 0, f, arg, NULL); }
static struct node *assign(const char *s, struct node *v) { return mk(N_ASSIGN, s, 0, v, NULL, NULL); }
static struct node *expr_stmt(struct node *e) { return mk(N_EXPR, NULL, 0, e, NULL, NULL); }
static struct node *if_stmt(struct node *t, struct node *body, struct node *orelse) { return mk(N_IF, NULL, 0, t, body, orelse); }
static struct node *while_stmt(struct node *t, struct node *body) { return mk(N_WHILE, NULL, 0, t, body, NULL); }
static struct node *ret(struct node *e) { return mk(N_RETURN, NULL, 0, e, NULL, NULL); }
static struct node *def(const char *s, const char *param, struct node *body) { return mk(N_DEF, s, 0, name(param), body, NULL); }

/* Chains statements into a list, the way a body is a sequence of statements. */
static struct node *body2(struct node *x, struct node *y)
{
    x->next = y;
    return x;
}
static struct node *body3(struct node *x, struct node *y, struct node *z) { return body2(x, body2(y, z)); }

static struct node *print_of(struct node *arg) { return expr_stmt(call(name("print"), arg)); }

static struct node *build_hello(void) { return print_of(str("Hello, world!")); }

static struct node *build_arithmetic(void)
{
    /* print((2 + 3) * 4 - 6) */
    return print_of(binop(NB_SUBTRACT, binop(NB_MULTIPLY, binop(NB_ADD, num(2), num(3)), num(4)), num(6)));
}

static struct node *build_branch(void)
{
    /* a = 3; b = 5; if a < b: print(b) else: print(a) */
    return body3(assign("a", num(3)), assign("b", num(5)),
                 if_stmt(lt(name("a"), name("b")), print_of(name("b")), print_of(name("a"))));
}

static struct node *build_loop(void)
{
    /* sum = 0; i = 1; while i < 11: sum = sum + i; i = i + 1 */
    struct node *body = body2(assign("sum", binop(NB_ADD, name("sum"), name("i"))),
                              assign("i", binop(NB_ADD, name("i"), num(1))));
    return body2(assign("sum", num(0)),
                 body3(assign("i", num(1)), while_stmt(lt(name("i"), num(11)), body), print_of(name("sum"))));
}

static struct node *build_call(void)
{
    /* def fact(n): if n < 2: return 1; return n * fact(n - 1) */
    struct node *test = lt(name("n"), num(2));
    struct node *rec = binop(NB_MULTIPLY, name("n"), call(name("fact"), binop(NB_SUBTRACT, name("n"), num(1))));
    struct node *fn_body = body2(if_stmt(test, ret(num(1)), NULL), ret(rec));
    return body2(def("fact", "n", fn_body), print_of(call(name("fact"), num(5))));
}

struct program {
    const char *name;
    struct node *(*build)(void);
};
static const struct program programs[] = {
    {"hello", build_hello}, {"arithmetic", build_arithmetic}, {"branch", build_branch},
    {"loop", build_loop},   {"call", build_call},
};

static const struct program *find_program(const char *example)
{
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        if (strcmp(programs[i].name, example) == 0) {
            return &programs[i];
        }
    }
    return NULL;
}

static void dump_expr(const struct node *n)
{
    switch (n->kind) {
    case N_INT:
        P("Constant(%ld)", (long)n->v);
        break;
    case N_STR:
        P("Constant('%s')", n->s);
        break;
    case N_NAME:
        P("Name(%s)", n->s);
        break;
    case N_BINOP:
        P("BinOp(");
        dump_expr(n->a);
        P(", %s, ", nb_names[n->v]);
        dump_expr(n->b);
        P(")");
        break;
    case N_COMPARE:
        P("Compare(");
        dump_expr(n->a);
        P(", Lt, ");
        dump_expr(n->b);
        P(")");
        break;
    default:
        require(n->kind == N_CALL, "expression kind");
        P("Call(");
        dump_expr(n->a);
        P(", [");
        dump_expr(n->b);
        P("])");
        break;
    }
}

static void dump_stmts(const struct node *n, int indent);

static void dump_stmt(const struct node *n, int indent)
{
    P("%*s", indent, "");
    switch (n->kind) {
    case N_ASSIGN:
        P("Assign(%s, ", n->s);
        dump_expr(n->a);
        P(")\n");
        break;
    case N_EXPR:
        P("Expr(");
        dump_expr(n->a);
        P(")\n");
        break;
    case N_RETURN:
        P("Return(");
        dump_expr(n->a);
        P(")\n");
        break;
    case N_IF:
        P("If(");
        dump_expr(n->a);
        P(")\n");
        dump_stmts(n->b, indent + 2);
        if (n->c) {
            P("%*selse:\n", indent, "");
            dump_stmts(n->c, indent + 2);
        }
        break;
    case N_WHILE:
        P("While(");
        dump_expr(n->a);
        P(")\n");
        dump_stmts(n->b, indent + 2);
        break;
    default:
        require(n->kind == N_DEF, "statement kind");
        P("FunctionDef(%s, args=[%s])\n", n->s, n->a->s);
        dump_stmts(n->b, indent + 2);
        break;
    }
}

static void dump_stmts(const struct node *n, int indent)
{
    for (; n != NULL; n = n->next) {
        dump_stmt(n, indent);
    }
}

static struct node *module_body;

static void stage_ast(const struct program *program)
{
    module_body = program->build();
    P("== stage_ast ==\nModule\n");
    dump_stmts(module_body, 2);
}

/* ------------------------------------------------------------------------------------------
 * stage_symtable
 *
 * symtable.c walks the tree once and records, for every name in every scope, how it is used.
 * A name that is bound in the scope, or is a parameter, is local to it. A name that is only
 * read is a global that is found implicitly. The code generator then picks the opcode family
 * from the scope: a function uses LOAD_FAST for its locals and LOAD_GLOBAL for the rest, and
 * a module, which is not optimized, uses the dictionary ops LOAD_NAME and STORE_NAME.
 * ---------------------------------------------------------------------------------------- */

enum { SYM_PARAM = 1, SYM_ASSIGNED = 2, SYM_USED = 4 };
enum { SC_LOCAL, SC_GLOBAL_IMPLICIT };

struct sym {
    const char *name;
    int flags;
};
struct scope {
    const char *name;
    int is_function;
    struct sym syms[MAX_SYMS];
    int nsyms;
};

static struct scope scopes[MAX_SCOPES];
static int nscopes;

static struct scope *new_scope(const char *scope_name, int is_function)
{
    require(nscopes < MAX_SCOPES, "more scopes");
    struct scope *sc = &scopes[nscopes++];
    sc->name = scope_name;
    sc->is_function = is_function;
    return sc;
}

static void add_sym(struct scope *sc, const char *sym_name, int flags)
{
    int i = 0;
    while (i < sc->nsyms && strcmp(sc->syms[i].name, sym_name) != 0) {
        i++;
    }
    if (i == sc->nsyms) {
        require(sc->nsyms < MAX_SYMS, "more symbols");
        sc->syms[sc->nsyms++] = (struct sym){sym_name, 0};
    }
    sc->syms[i].flags |= flags;
}

static int scope_kind(const struct scope *sc, const char *sym_name)
{
    int i = 0;
    while (strcmp(sc->syms[i].name, sym_name) != 0) {
        i++;
    }
    return (sc->syms[i].flags & (SYM_PARAM | SYM_ASSIGNED)) ? SC_LOCAL : SC_GLOBAL_IMPLICIT;
}

static void symtable_expr(struct scope *sc, const struct node *n)
{
    switch (n->kind) {
    case N_NAME:
        add_sym(sc, n->s, SYM_USED);
        break;
    case N_BINOP:
    case N_COMPARE:
        symtable_expr(sc, n->a);
        symtable_expr(sc, n->b);
        break;
    case N_CALL:
        symtable_expr(sc, n->a);
        symtable_expr(sc, n->b);
        break;
    default: /* constants */
        break;
    }
}

static void symtable_stmts(struct scope *sc, struct node *n)
{
    for (; n != NULL; n = n->next) {
        switch (n->kind) {
        case N_ASSIGN:
            symtable_expr(sc, n->a);
            add_sym(sc, n->s, SYM_ASSIGNED);
            break;
        case N_EXPR:
        case N_RETURN:
            symtable_expr(sc, n->a);
            break;
        case N_IF:
            symtable_expr(sc, n->a);
            symtable_stmts(sc, n->b);
            symtable_stmts(sc, n->c);
            break;
        case N_WHILE:
            symtable_expr(sc, n->a);
            symtable_stmts(sc, n->b);
            break;
        default: { /* a def: the name is bound here, and the body is a new scope */
            add_sym(sc, n->s, SYM_ASSIGNED);
            n->v = nscopes;
            struct scope *inner = new_scope(n->s, 1);
            add_sym(inner, n->a->s, SYM_PARAM);
            symtable_stmts(inner, n->b);
            break;
        }
        }
    }
}

static void stage_symtable(void)
{
    symtable_stmts(new_scope("<module>", 0), module_body);
    P("== stage_symtable ==\n");
    for (int i = 0; i < nscopes; i++) {
        const struct scope *sc = &scopes[i];
        P("scope %s (%s)\n", sc->name, sc->is_function ? "function" : "module");
        for (int j = 0; j < sc->nsyms; j++) {
            P("  %-6s %s\n", sc->syms[j].name,
              scope_kind(sc, sc->syms[j].name) == SC_LOCAL ? "LOCAL" : "GLOBAL_IMPLICIT");
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Code units. compile.c keeps one per function body or module, with the instruction sequence
 * and the tables that become co_consts, co_names and co_varnames.
 * ---------------------------------------------------------------------------------------- */

enum { K_INT, K_STR, K_NONE, K_CODE };
struct konst {
    int kind;
    int64_t i;
    const char *s;
};

struct unit {
    const char *name;
    const struct scope *sc;
    struct ins seq[MAX_SEQ];
    int nseq, nlabels;
    const char *names[MAX_NAMES];
    int nnames;
    const char *varname;
    struct konst consts[MAX_CONSTS];
    int nconsts;
    struct block blocks[MAX_BLOCKS];
    int nblocks;
    struct block *entry;
    int stacksize;
    uint8_t code[MAX_CODE];
    int ncode;
};

static struct unit units[MAX_UNITS];
static int nunits;

static int name_index(struct unit *u, const char *n)
{
    int i = 0;
    while (i < u->nnames && strcmp(u->names[i], n) != 0) {
        i++;
    }
    if (i == u->nnames) {
        require(u->nnames < MAX_NAMES, "more names");
        u->names[u->nnames++] = n;
    }
    return i;
}

/* Equal constants share one slot, like the const cache of the real compiler. */
static int const_index(struct unit *u, int kind, int64_t i, const char *s)
{
    int k = 0;
    while (k < u->nconsts &&
           !((u->consts[k].kind == kind) & (u->consts[k].i == i) & (u->consts[k].s == s))) {
        k++;
    }
    if (k == u->nconsts) {
        require(u->nconsts < MAX_CONSTS, "more constants");
        u->consts[u->nconsts++] = (struct konst){kind, i, s};
    }
    return k;
}

enum { M_SEQ, M_CFG, M_FINAL };

static void print_const(const struct konst *k)
{
    switch (k->kind) {
    case K_INT:
        P("%ld", (long)k->i);
        break;
    case K_STR:
        P("'%s'", k->s);
        break;
    case K_NONE:
        P("None");
        break;
    default:
        P("<code %s>", units[k->i].name);
        break;
    }
}

/* One instruction on a line, in the style of dis.dis. */
static void print_ins(const struct unit *u, const struct ins *x, int mode)
{
    const struct opinfo *o = &ops[x->op];
    P("  %s", o->name);
    if (o->hasarg && !(o->isjump && mode != M_FINAL)) {
        P(" %d", x->arg);
    }
    switch (x->op) {
    case O_LOAD_CONST:
        P(" (");
        print_const(&u->consts[x->arg]);
        P(")");
        break;
    case O_LOAD_NAME:
    case O_STORE_NAME:
        P(" (%s)", u->names[x->arg]);
        break;
    case O_LOAD_GLOBAL:
        P(" (%s%s)", u->names[x->arg >> 1], (x->arg & 1) ? " + NULL" : "");
        break;
    case O_LOAD_FAST:
    case O_LOAD_FAST_BORROW:
        P(" (%s)", u->varname);
        break;
    case O_COMPARE_OP:
        P((x->arg & CMP_TO_BOOL) ? " (bool(<))" : " (<)");
        break;
    case O_BINARY_OP:
        P(" (%s)", nb_syms[x->arg]);
        break;
    case O_LOAD_COMMON_CONSTANT:
        P(" (None)");
        break;
    default:
        break;
    }
    if (o->isjump) {
        if (mode == M_SEQ) {
            P(" -> L%d", x->arg);
        } else if (mode == M_CFG) {
            P(" -> B%d", x->target->id);
        } else {
            P(" (to %d)", x->target->offset);
        }
    }
    P("\n");
}

/* ------------------------------------------------------------------------------------------
 * stage_codegen
 *
 * codegen.c appends instructions to the unit's sequence while it walks the tree, and marks
 * jump targets with labels. Constants are loaded with LOAD_CONST, a condition is a value plus
 * TO_BOOL plus a conditional jump, and a call is the callable, a PUSH_NULL for the self slot,
 * the arguments, and CALL. A function body is compiled as its own unit first, and the parent
 * then loads the finished code object and makes a function out of it.
 * ---------------------------------------------------------------------------------------- */

static int compile_unit(const char *unit_name, const struct scope *sc, struct node *body);

static void emit(struct unit *u, int op, int arg)
{
    require(u->nseq < MAX_SEQ, "longer sequence");
    u->seq[u->nseq++] = (struct ins){op, arg, NULL, 0};
}

static int new_label(struct unit *u)
{
    require(u->nlabels < MAX_LABELS, "more labels");
    return u->nlabels++;
}

static void use_label(struct unit *u, int label) { emit(u, O_LABEL, label); }

static void nameop(struct unit *u, const char *n, int store)
{
    int kind = scope_kind(u->sc, n);
    if (u->sc->is_function && kind == SC_LOCAL) {
        require(!store, "a store to a local of a function");
        emit(u, O_LOAD_FAST, 0);
    } else if (u->sc->is_function) {
        require(!store, "a store to a global from a function");
        emit(u, O_LOAD_GLOBAL, name_index(u, n) << 1);
    } else {
        emit(u, store ? O_STORE_NAME : O_LOAD_NAME, name_index(u, n));
    }
}

static void gen_expr(struct unit *u, const struct node *n)
{
    switch (n->kind) {
    case N_INT:
        emit(u, O_LOAD_CONST, const_index(u, K_INT, n->v, NULL));
        break;
    case N_STR:
        emit(u, O_LOAD_CONST, const_index(u, K_STR, 0, n->s));
        break;
    case N_NAME:
        nameop(u, n->s, 0);
        break;
    case N_BINOP:
        gen_expr(u, n->a);
        gen_expr(u, n->b);
        emit(u, O_BINARY_OP, (int)n->v);
        break;
    case N_COMPARE:
        gen_expr(u, n->a);
        gen_expr(u, n->b);
        emit(u, O_COMPARE_OP, CMP_LT);
        break;
    default:
        require(n->kind == N_CALL, "expression kind");
        gen_expr(u, n->a);
        emit(u, O_PUSH_NULL, 0);
        gen_expr(u, n->b);
        emit(u, O_CALL, 1);
        break;
    }
}

/* codegen_jump_if for the general case: jump to label when the test is false. */
static void gen_jump_if_false(struct unit *u, const struct node *test, int label)
{
    gen_expr(u, test);
    emit(u, O_TO_BOOL, 0);
    emit(u, O_POP_JUMP_IF_FALSE, label);
}

static void gen_stmts(struct unit *u, const struct node *n);

static void gen_stmt(struct unit *u, const struct node *n)
{
    switch (n->kind) {
    case N_ASSIGN:
        gen_expr(u, n->a);
        nameop(u, n->s, 1);
        break;
    case N_EXPR:
        gen_expr(u, n->a);
        emit(u, O_POP_TOP, 0);
        break;
    case N_RETURN:
        gen_expr(u, n->a);
        emit(u, O_RETURN_VALUE, 0);
        break;
    case N_IF: {
        int end = new_label(u);
        int next = end;
        if (n->c) {
            next = new_label(u);
        }
        gen_jump_if_false(u, n->a, next);
        gen_stmts(u, n->b);
        if (n->c) {
            emit(u, O_JUMP_NO_INTERRUPT, end);
            use_label(u, next);
            gen_stmts(u, n->c);
        }
        use_label(u, end);
        break;
    }
    case N_WHILE: {
        int loop = new_label(u);
        int end = new_label(u);
        int anchor = new_label(u);
        use_label(u, loop);
        gen_jump_if_false(u, n->a, anchor);
        gen_stmts(u, n->b);
        emit(u, O_JUMP, loop);
        use_label(u, anchor);
        use_label(u, end);
        break;
    }
    default: { /* a def */
        require(n->kind == N_DEF, "statement kind");
        int code = compile_unit(n->s, &scopes[n->v], n->b);
        emit(u, O_LOAD_CONST, const_index(u, K_CODE, code, NULL));
        emit(u, O_MAKE_FUNCTION, 0);
        nameop(u, n->s, 1);
        break;
    }
    }
}

static void gen_stmts(struct unit *u, const struct node *n)
{
    for (; n != NULL; n = n->next) {
        gen_stmt(u, n);
    }
}

static void stage_codegen(struct unit *u, struct node *body)
{
    emit(u, O_RESUME, 0);
    gen_stmts(u, body);
    /* _PyCodegen_AddReturnAtEnd: falling off the end returns None. */
    emit(u, O_LOAD_CONST, const_index(u, K_NONE, 0, NULL));
    emit(u, O_RETURN_VALUE, 0);
    P("== stage_codegen: %s ==\n", u->name);
    for (int i = 0; i < u->nseq; i++) {
        if (u->seq[i].op == O_LABEL) {
            P(" L%d:\n", u->seq[i].arg);
        } else {
            print_ins(u, &u->seq[i], M_SEQ);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_cfg
 *
 * _PyCfg_FromInstructionSequence cuts the sequence into basic blocks. A label starts a block,
 * and so does the instruction after a jump or a return. A jump then points at a block instead
 * of at a label. A block that no label and no jump reaches, such as the second return that
 * follows a function's own return, is still there until the optimizer removes it.
 * ---------------------------------------------------------------------------------------- */

static struct block *new_block(struct unit *u)
{
    require(u->nblocks < MAX_BLOCKS, "more blocks");
    struct block *b = &u->blocks[u->nblocks];
    b->id = u->nblocks++;
    return b;
}

static struct block *start_next_block(struct unit *u, struct block *cur)
{
    struct block *b = new_block(u);
    cur->next = b;
    return b;
}

static void dump_blocks(const struct unit *u)
{
    for (const struct block *b = u->entry; b != NULL; b = b->next) {
        P(" B%d:\n", b->id);
        for (int i = 0; i < b->n; i++) {
            print_ins(u, &b->in[i], M_CFG);
        }
    }
}

static void stage_cfg(struct unit *u)
{
    struct block *label_block[MAX_LABELS];
    struct block *cur = new_block(u);
    u->entry = cur;
    for (int i = 0; i < u->nseq; i++) {
        const struct ins *x = &u->seq[i];
        if (x->op == O_LABEL) {
            if (cur->n > 0) {
                cur = start_next_block(u, cur);
            }
            label_block[x->arg] = cur;
            continue;
        }
        if (cur->n > 0 && is_block_end(cur->in[cur->n - 1].op)) {
            cur = start_next_block(u, cur);
        }
        require(cur->n < MAX_BI, "longer block");
        cur->in[cur->n++] = *x;
    }
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            if (ops[b->in[i].op].isjump) {
                b->in[i].target = label_block[b->in[i].arg];
            }
        }
    }
    P("== stage_cfg: %s ==\n", u->name);
    dump_blocks(u);
}

/* ------------------------------------------------------------------------------------------
 * stage_optimize_cfg
 *
 * optimize_cfg runs these passes on the blocks, in this order. The ones that the examples
 * never trigger are left out, and FIDELITY.md lists them.
 * ---------------------------------------------------------------------------------------- */

/* inline_small_or_no_lineno_blocks: a jump to a short block that returns is replaced by a copy
 * of that block, which saves the jump at run time. The jump becomes a NOP. */
static void inline_small_exit_blocks(struct unit *u)
{
    int changes;
    do {
        changes = 0;
        for (struct block *b = u->entry; b != NULL; b = b->next) {
            struct ins *last = &b->in[b->n - 1];
            if (is_uncond(last->op) && last->op != O_RETURN_VALUE) {
                struct block *t = last->target;
                if ((t->in[t->n - 1].op == O_RETURN_VALUE) & (t->n <= MAX_COPY_SIZE)) {
                    require(b->n + t->n <= MAX_BI, "longer block");
                    last->op = O_NOP;
                    last->target = NULL;
                    memcpy(&b->in[b->n], t->in, (size_t)t->n * sizeof t->in[0]);
                    b->n += t->n;
                    changes = 1;
                }
            }
        }
    } while (changes);
}

static void mark_reachable(struct block *b)
{
    if (b->visited) {
        return;
    }
    b->visited = 1;
    for (int i = 0; i < b->n; i++) {
        if (b->in[i].target) {
            mark_reachable(b->in[i].target);
        }
    }
    if (has_fallthrough(b)) {
        mark_reachable(b->next);
    }
}

/* remove_unreachable: drop every block that no jump and no fallthrough reaches. */
static void remove_unreachable(struct unit *u)
{
    for (int i = 0; i < u->nblocks; i++) {
        u->blocks[i].visited = 0;
    }
    mark_reachable(u->entry);
    struct block **link = &u->entry;
    while (*link != NULL) {
        if ((*link)->visited) {
            link = &(*link)->next;
        } else {
            *link = (*link)->next;
        }
    }
}

/* optimize_load_const: a constant that fits an instruction argument is loaded by an
 * instruction of its own, with no trip through the constants table. */
static void optimize_load_const(struct unit *u)
{
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            struct ins *x = &b->in[i];
            if (x->op != O_LOAD_CONST) {
                continue;
            }
            const struct konst *k = &u->consts[x->arg];
            if (k->kind == K_INT) {
                require((k->i >= 0) & (k->i <= 255), "a large integer constant");
                x->op = O_LOAD_SMALL_INT;
                x->arg = (int)k->i;
            } else if (k->kind == K_NONE) {
                x->op = O_LOAD_COMMON_CONSTANT;
                x->arg = CONSTANT_NONE;
            }
        }
    }
}

/* optimize_basic_block: the peephole rewrites of one block. */
static void optimize_basic_block(struct block *b)
{
    for (int i = 0; i < b->n; i++) {
        struct ins *x = &b->in[i];
        int nextop = i + 1 < b->n ? b->in[i + 1].op : -1;
        switch (x->op) {
        case O_LOAD_GLOBAL:
            /* LOAD_GLOBAL; PUSH_NULL becomes one instruction with the low bit set. */
            require(nextop == O_PUSH_NULL, "a global that is not called");
            x->arg |= 1;
            b->in[i + 1].op = O_NOP;
            break;
        case O_COMPARE_OP:
            /* COMPARE_OP; TO_BOOL: the comparison produces a bool itself. The result is
             * visited again as a COMPARE_OP, and has nothing left to fold. */
            if (x->arg & CMP_TO_BOOL) {
                break;
            }
            require(nextop == O_TO_BOOL, "a comparison that is not a condition");
            x->op = O_NOP;
            b->in[i + 1].op = O_COMPARE_OP;
            b->in[i + 1].arg = x->arg | CMP_TO_BOOL;
            break;
        case O_BINARY_OP: {
            /* fold_const_binop: two constants before the operation are computed now. */
            int found[2], nfound = 0;
            for (int j = i - 1; nfound < 2; j--) {
                require(j >= 0, "operands in another block");
                if (b->in[j].op != O_LOAD_SMALL_INT) {
                    break;
                }
                found[nfound++] = j;
            }
            if (nfound == 2) {
                int l = b->in[found[1]].arg, r = b->in[found[0]].arg, v;
                switch (x->arg) {
                case NB_ADD:
                    v = l + r;
                    break;
                case NB_MULTIPLY:
                    v = l * r;
                    break;
                default:
                    require(x->arg == NB_SUBTRACT, "a binary operator");
                    v = l - r;
                    break;
                }
                require((v >= 0) & (v <= 255), "a folded constant that is not a small int");
                b->in[found[0]].op = O_NOP;
                b->in[found[1]].op = O_NOP;
                x->op = O_LOAD_SMALL_INT;
                x->arg = v;
            }
            break;
        }
        default:
            break;
        }
    }
}

/* remove_redundant_nops: the NOPs that the passes above left behind are deleted. */
static void remove_nops(struct unit *u)
{
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        int kept = 0;
        for (int i = 0; i < b->n; i++) {
            if (b->in[i].op != O_NOP) {
                b->in[kept++] = b->in[i];
            }
        }
        b->n = kept;
    }
}

/* remove_unused_consts: constants that no instruction loads any more are dropped, except slot 0,
 * which a function keeps for its docstring. The remaining LOAD_CONSTs are renumbered. */
static void remove_unused_consts(struct unit *u)
{
    int used[MAX_CONSTS] = {1}, map[MAX_CONSTS];
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            if (b->in[i].op == O_LOAD_CONST) {
                used[b->in[i].arg] = 1;
            }
        }
    }
    int kept = 0;
    for (int k = 0; k < u->nconsts; k++) {
        if (used[k]) {
            map[k] = kept;
            u->consts[kept++] = u->consts[k];
        }
    }
    u->nconsts = kept;
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            if (b->in[i].op == O_LOAD_CONST) {
                b->in[i].arg = map[b->in[i].arg];
            }
        }
    }
}

static void dump_consts(const struct unit *u)
{
    P("consts:");
    for (int k = 0; k < u->nconsts; k++) {
        P(" ");
        print_const(&u->consts[k]);
    }
    P("\n");
}

static void stage_optimize_cfg(struct unit *u)
{
    P("== stage_optimize_cfg: %s ==\n", u->name);
    inline_small_exit_blocks(u);
    P("after inline_small_or_no_lineno_blocks:\n");
    dump_blocks(u);
    remove_unreachable(u);
    P("after remove_unreachable:\n");
    dump_blocks(u);
    optimize_load_const(u);
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        optimize_basic_block(b);
    }
    remove_nops(u);
    P("after optimize_load_const, optimize_basic_block, remove_redundant_nops:\n");
    dump_blocks(u);
    remove_unused_consts(u);
    dump_consts(u);
}

/* ------------------------------------------------------------------------------------------
 * stage_assemble
 *
 * The optimizer's blocks become the final instructions in this order: the stack depth is
 * computed, forward conditional jumps get a NOT_TAKEN after them, loads of locals that cannot
 * outlive their frame slot become borrowing loads, and the assembler assigns each
 * instruction an offset in code units. Every instruction takes one unit plus its inline cache
 * entries, and a jump's argument is the distance to its target, counted from the end of the
 * jump and its caches. A jump back becomes JUMP_BACKWARD. The bytes follow, two per unit,
 * the opcode first.
 * ---------------------------------------------------------------------------------------- */

static int maxdepth;

/* calculate_stackdepth: walk the blocks and record the deepest stack. */
static void walk_depth(struct block *b, int depth)
{
    if (b->startdepth >= 0) {
        return;
    }
    b->startdepth = depth;
    for (int i = 0; i < b->n; i++) {
        const struct ins *x = &b->in[i];
        if (depth > maxdepth) {
            maxdepth = depth;
        }
        int after = depth + pushes(x) - pops(x);
        if (ops[x->op].isjump) {
            walk_depth(x->target, after);
        }
        depth = after;
    }
    if (has_fallthrough(b)) {
        walk_depth(b->next, depth);
    }
}

static void stage_assemble(struct unit *u)
{
    for (int i = 0; i < u->nblocks; i++) {
        u->blocks[i].startdepth = -1;
        u->blocks[i].visited = 0;
    }
    maxdepth = 0;
    walk_depth(u->entry, 0);
    u->stacksize = maxdepth;

    /* normalize_jumps: a forward conditional jump is followed by NOT_TAKEN. A block is marked
     * when it is reached, so an unmarked target lies ahead. */
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        b->visited = 1;
        struct ins *last = &b->in[b->n - 1];
        if (last->op == O_POP_JUMP_IF_FALSE) {
            require(!last->target->visited, "a backward conditional jump");
            require(b->n < MAX_BI, "longer block");
            b->in[b->n++] = (struct ins){O_NOT_TAKEN, 0, NULL, 0};
        }
    }

    /* optimize_load_fast: with no store to a local and nothing left on the stack across a
     * block, every load of a local can borrow the reference held by the frame. */
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        require(b->startdepth == 0, "a value on the stack across blocks");
        for (int i = 0; i < b->n; i++) {
            if (b->in[i].op == O_LOAD_FAST) {
                b->in[i].op = O_LOAD_FAST_BORROW;
            }
        }
    }

    /* resolve_unconditional_jumps: the only jump that remains goes back. */
    int pos = 0;
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        b->pos = pos++;
    }
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            struct ins *x = &b->in[i];
            if (x->op == O_JUMP) {
                require(x->target->pos <= b->pos, "a forward jump");
                x->op = O_JUMP_BACKWARD;
            }
        }
    }

    /* resolve_jump_offsets: an offset for every instruction, then the jump arguments. */
    int offset = 0;
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        b->offset = offset;
        for (int i = 0; i < b->n; i++) {
            b->in[i].offset = offset;
            offset += 1 + ops[b->in[i].op].caches;
        }
    }
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            struct ins *x = &b->in[i];
            if (ops[x->op].isjump) {
                int after = x->offset + 1 + ops[x->op].caches;
                require((x->op == O_POP_JUMP_IF_FALSE) | (x->op == O_JUMP_BACKWARD), "a jump kind");
                x->arg = x->op == O_JUMP_BACKWARD ? after - x->target->offset : x->target->offset - after;
                require(x->arg < 256, "a jump over more than 255 units");
            }
        }
    }

    /* assemble_emit: opcode and argument bytes, then zeroed cache entries. */
    for (struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            const struct ins *x = &b->in[i];
            require(x->arg < 256, "an argument over 255");
            require(u->ncode + 2 * (1 + ops[x->op].caches) <= MAX_CODE, "longer code");
            u->code[u->ncode++] = (uint8_t)ops[x->op].code;
            u->code[u->ncode++] = (uint8_t)x->arg;
            for (int c = 0; c < ops[x->op].caches; c++) {
                u->code[u->ncode++] = 0;
                u->code[u->ncode++] = 0;
            }
        }
    }

    P("== stage_assemble: %s ==\n", u->name);
    for (const struct block *b = u->entry; b != NULL; b = b->next) {
        for (int i = 0; i < b->n; i++) {
            P("%4d", b->in[i].offset);
            print_ins(u, &b->in[i], M_FINAL);
        }
    }
    P("stacksize: %d\n", u->stacksize);
    dump_consts(u);
    P("names:");
    for (int k = 0; k < u->nnames; k++) {
        P(" %s", u->names[k]);
    }
    P("\ncode:");
    for (int k = 0; k < u->ncode; k++) {
        P(" %02x", u->code[k]);
    }
    P("\n");
}

static int compile_unit(const char *unit_name, const struct scope *sc, struct node *body)
{
    require(nunits < MAX_UNITS, "more code units");
    int index = nunits++;
    struct unit *u = &units[index];
    u->name = unit_name;
    u->sc = sc;
    if (sc->is_function) {
        u->varname = sc->syms[0].name; /* the only parameter, which is the first symbol */
    }
    stage_codegen(u, body);
    stage_cfg(u);
    stage_optimize_cfg(u);
    stage_assemble(u);
    return index;
}

/* ------------------------------------------------------------------------------------------
 * stage_run
 *
 * The eval loop. A frame has fast locals, and a value stack whose size the compiler computed.
 * Values are tagged. The stack holds a callable and a NULL slot below the arguments of a call,
 * as the compiler laid it out with PUSH_NULL. The trace shows every instruction that runs.
 * ---------------------------------------------------------------------------------------- */

enum { V_NULL, V_NONE, V_BOOL, V_INT, V_STR, V_CODE, V_FUNC, V_PRINT };
struct val {
    int kind;
    int64_t i;
    const char *s;
};

static struct {
    const char *name;
    struct val v;
} globals[MAX_NAMES];
static int nglobals;

static struct val lookup(const char *n)
{
    for (int i = 0; i < nglobals; i++) {
        if (strcmp(globals[i].name, n) == 0) {
            return globals[i].v;
        }
    }
    /* Not a module global: the only builtin is print. */
    require(strcmp(n, "print") == 0, "an unknown name");
    return (struct val){V_PRINT, 0, NULL};
}

static void store_global(const char *n, struct val v)
{
    int i = 0;
    while (i < nglobals && strcmp(globals[i].name, n) != 0) {
        i++;
    }
    if (i == nglobals) {
        require(nglobals < MAX_NAMES, "more globals");
        globals[nglobals++].name = n;
    }
    globals[i].v = v;
}

static struct val eval_frame(const struct unit *u, const struct val *args, int depth)
{
    struct val stack[MAX_STACK], fast[1];
    int sp = 0;
    int pc = 0;
    require(u->stacksize <= MAX_STACK, "a deeper stack");
    fast[0] = args[0];
    for (;;) {
        int code = u->code[2 * pc], arg = u->code[2 * pc + 1];
        int op = 0;
        while (ops[op].code != code) {
            op++;
        }
        P("%*s%3d %s %d  [%d]\n", 2 * depth, "", pc, ops[op].name, arg, sp);
        pc += 1 + ops[op].caches;
        switch (op) {
        case O_RESUME:
        case O_NOT_TAKEN:
            break;
        case O_LOAD_FAST_BORROW:
            stack[sp++] = fast[arg];
            break;
        case O_LOAD_SMALL_INT:
            stack[sp++] = (struct val){V_INT, arg, NULL};
            break;
        case O_LOAD_COMMON_CONSTANT:
            stack[sp++] = (struct val){V_NONE, 0, NULL};
            break;
        case O_LOAD_CONST: {
            const struct konst *k = &u->consts[arg];
            stack[sp++] = k->kind == K_STR ? (struct val){V_STR, 0, k->s} : (struct val){V_CODE, k->i, NULL};
            break;
        }
        case O_LOAD_NAME:
            stack[sp++] = lookup(u->names[arg]);
            break;
        case O_LOAD_GLOBAL:
            require(arg & 1, "a global that is not called");
            stack[sp++] = lookup(u->names[arg >> 1]);
            stack[sp++] = (struct val){V_NULL, 0, NULL};
            break;
        case O_STORE_NAME:
            store_global(u->names[arg], stack[--sp]);
            break;
        case O_PUSH_NULL:
            stack[sp++] = (struct val){V_NULL, 0, NULL};
            break;
        case O_POP_TOP:
            sp--;
            break;
        case O_MAKE_FUNCTION:
            stack[sp - 1].kind = V_FUNC;
            break;
        case O_COMPARE_OP:
            require(arg == (CMP_LT | CMP_TO_BOOL), "a comparison kind");
            sp--;
            stack[sp - 1] = (struct val){V_BOOL, stack[sp - 1].i < stack[sp].i, NULL};
            break;
        case O_BINARY_OP: {
            int64_t l = stack[sp - 2].i, r = stack[sp - 1].i, v;
            switch (arg) {
            case NB_ADD:
                v = l + r;
                break;
            case NB_MULTIPLY:
                v = l * r;
                break;
            default:
                require(arg == NB_SUBTRACT, "a binary operator");
                v = l - r;
                break;
            }
            sp--;
            stack[sp - 1] = (struct val){V_INT, v, NULL};
            break;
        }
        case O_POP_JUMP_IF_FALSE:
            if (!stack[--sp].i) {
                pc += arg;
            }
            break;
        case O_CALL: {
            struct val *argv = &stack[sp - arg];
            struct val callee = argv[-2], result = {V_NONE, 0, NULL};
            require(argv[-1].kind == V_NULL, "a bound method");
            if (callee.kind == V_PRINT) {
                if (argv[0].kind == V_STR) {
                    printf("%s\n", argv[0].s);
                } else {
                    printf("%ld\n", (long)argv[0].i);
                }
            } else {
                result = eval_frame(&units[callee.i], argv, depth + 1);
            }
            sp -= arg + 2;
            stack[sp++] = result;
            break;
        }
        case O_RETURN_VALUE:
            return stack[sp - 1];
        default:
            require(op == O_JUMP_BACKWARD, "an opcode");
            pc -= arg;
            break;
        }
    }
}

static void stage_run(void)
{
    P("== stage_run ==\n");
    struct val none = {V_NONE, 0, NULL};
    eval_frame(&units[0], &none, 0);
}

int main(int argc, char **argv)
{
    const struct program *program = NULL;
    if (argc < 2 || (program = find_program(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_ast(program);
    stage_symtable();
    compile_unit("<module>", &scopes[0], module_body);
    stage_run();
    return 0;
}
