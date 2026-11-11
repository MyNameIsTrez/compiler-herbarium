/*
 * lean: a cut-down version of the reference counting passes of the Lean 4 compiler.
 *
 * Lean programs are pure and use immutable data, yet they update lists and trees in place. The
 * compiler makes that safe with reference counts that it inserts itself, and then removes most
 * of them. After lowering, a function is in A-normal form over constructors, projections, calls
 * and `case`. Four passes follow, from "Counting Immutable Beans" (Ullrich and de Moura, 2019):
 *
 *   reset and reuse   when a constructor cell dies and a cell of the same shape is built soon
 *                     after, `reset x` / `reuse w` mark that the memory may be recycled
 *   borrow inference  a fixpoint that marks a parameter as borrowed unless the function has to
 *                     own it, so that callers skip the inc before and the dec after a call
 *   explicit RC       a backward liveness walk inserts `inc` and `dec` so that every owned
 *                     value is consumed exactly once; a value read out of a live object (a
 *                     "derived borrow") needs no inc of its own
 *   expand            `reset`/`reuse` becomes a test of `isShared`: if the cell is exclusive,
 *                     it is updated in place and the incs on its fields are skipped, otherwise
 *                     a fresh cell is allocated
 *
 * The interpreter at the end runs the final form on a heap that counts references and checks
 * them: a use after free, a reference operation on a value that is not a reference, or a leak
 * at exit stops it with an error. The result is printed on stdout.
 *
 * The parser, the type checker and every pass before the reference counting ones are not here:
 * each example is a few functions written out directly in the IR, next to the Lua program that
 * computes the same result.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the result on stdout and the program after each stage on stderr. The program is
 *   interpreted, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source       the functions of the example, before any reference counting
 *   stage_reset_reuse  insert reset and reuse (functions R, D and S of the paper)
 *   stage_borrow       infer which parameters are borrowed
 *   stage_rc           insert inc and dec
 *   stage_expand       expand reset and reuse into an isShared test and join points
 *   stage_run          run the program on a counting heap and check the counts
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXV 48 /* variables in one function; the live sets are 64-bit masks */
#define MAXARGS 3
#define MAXFIELDS 3
#define MAXJP 4
#define NALT 2
#define POOL 2048
#define HEAP 256

typedef uint64_t Set;
#define BIT(v) ((Set)1 << (v))
#define HAS(s, v) (((s) >> (v)) & 1)

/* ---------------------------------------------------------------------------------------- */
/* The IR                                                                                   */
/* ---------------------------------------------------------------------------------------- */

enum ty { T_SCALAR, T_OBJ, T_REF }; /* never a pointer; pointer or boxed scalar; always a pointer */
enum kind { K_LET, K_CASE, K_RET, K_INC, K_DEC, K_DEL, K_OSET, K_JP, K_JMP };
enum vkind { V_LIT, V_CTOR, V_PROJ, V_CALL, V_RESET, V_REUSE, V_ISSHARED };

enum { TY_LIST, TY_TREE, TY_PAIR, TY_BOOL };
enum { C_NIL, C_CONS, C_LEAF, C_NODE, C_PAIR, C_FALSE, C_TRUE };
enum { F_ADD, F_LT, F_LENGTH, F_MAPINC, F_REV, F_APPEND, F_INSERT, F_DUP, F_HEADLEN, F_FILTER, F_BUMP, F_SWAP, F_UNCONS, F_ZIPADD, F_TAILLEN, F_MAIN, NFN };

struct ctor {
    const char *name;
    int type, tag, size; /* size is the number of fields; a constructor without fields is a scalar */
};

static const struct ctor ctors[] = {
    {"nil", TY_LIST, 0, 0},  {"cons", TY_LIST, 1, 2},  {"leaf", TY_TREE, 0, 0}, {"node", TY_TREE, 1, 3},
    {"pair", TY_PAIR, 0, 2}, {"false", TY_BOOL, 0, 0}, {"true", TY_BOOL, 1, 0},
};

typedef struct Code Code;
typedef struct {
    int ctor;
    Code *body;
} Alt;

/*
 * One node of a function body. A body is a chain of nodes linked by next, ending in a case, ret
 * or jmp. The fields mean different things per kind:
 *   LET     x := value; vk is the kind of value; a is the literal, the constructor, the field,
 *           the field count (reset) or the callee; y is the object read (proj, reset,
 *           isShared) or the token (reuse); flag on reuse says the tag changes
 *   CASE    x is the scrutinee, alts the branches
 *   RET     x
 *   INC/DEC x, a the count, flag says "check that x is a pointer first"
 *   DEL     x (free the cell, leave its fields alone), OSET x[a] := y
 *   JP      block x with parameters args[0..nargs), body, then next is the code in its scope
 *   JMP     jump to block x with args; an argument of -1 is erased
 */
struct Code {
    int kind, x, vk, a, y, flag;
    int nargs, args[MAXARGS];
    Alt alts[NALT];
    Code *body, *next;
};

typedef struct {
    const char *name;
    int ext; /* a builtin without a body */
    int np, pty[2], borrow[2];
    Code *body;
    int nv, vty[MAXV], njp;
} Fn;

static Fn fns[NFN];
static Code pool[POOL];
static int npool;

static Code *node(int kind)
{
    Code *c = &pool[npool++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    return c;
}

/* ---- builders: the hardcoded input is written with these ---- */

static Fn *bf; /* the function being built */

#define A(...) ((int[]){__VA_ARGS__}), ((int)(sizeof((int[]){__VA_ARGS__}) / sizeof(int)))
#define NO NULL, 0

static Fn *fn_begin(int id, const char *name, int np, int t0, int t1)
{
    Fn *f = &fns[id];
    memset(f, 0, sizeof *f);
    f->name = name;
    f->np = np;
    f->pty[0] = t0;
    f->pty[1] = t1;
    for (int i = 0; i < np; i++)
        f->vty[i] = f->pty[i];
    f->nv = np;
    bf = f;
    return f;
}

static Code *b_let(int x, int ty, int vk)
{
    Code *c = node(K_LET);
    c->x = x;
    c->vk = vk;
    bf->vty[x] = ty;
    if (x >= bf->nv)
        bf->nv = x + 1;
    return c;
}

static void set_args(Code *c, const int *a, int n)
{
    c->nargs = n;
    for (int i = 0; i < n; i++)
        c->args[i] = a[i];
}

static Code *b_lit(int x, int v)
{
    Code *c = b_let(x, T_SCALAR, V_LIT);
    c->a = v;
    return c;
}

static Code *b_ctor(int x, int ctor, const int *a, int n)
{
    Code *c = b_let(x, ctors[ctor].size > 0 ? T_REF : T_SCALAR, V_CTOR);
    c->a = ctor;
    set_args(c, a, n);
    return c;
}

static Code *b_proj(int x, int ty, int i, int y)
{
    Code *c = b_let(x, ty, V_PROJ);
    c->a = i;
    c->y = y;
    return c;
}

static Code *b_call(int x, int ty, int callee, const int *a, int n)
{
    Code *c = b_let(x, ty, V_CALL);
    c->a = callee;
    set_args(c, a, n);
    return c;
}

static Code *b_ret(int x)
{
    Code *c = node(K_RET);
    c->x = x;
    return c;
}

static Code *b_case(int x, int c0, Code *b0, int c1, Code *b1)
{
    Code *c = node(K_CASE);
    c->x = x;
    c->alts[0] = (Alt){c0, b0};
    c->alts[1] = (Alt){c1, b1};
    return c;
}

/* Link the nodes (or chains) in order. */
static Code *seq(Code *first, ...)
{
    va_list ap;
    Code *tail = first;
    va_start(ap, first);
    for (Code *c = va_arg(ap, Code *); c != NULL; c = va_arg(ap, Code *)) {
        while (tail->next != NULL)
            tail = tail->next;
        tail->next = c;
    }
    va_end(ap);
    return first;
}

/* ---------------------------------------------------------------------------------------- */
/* Printing                                                                                 */
/* ---------------------------------------------------------------------------------------- */

static const char *ty_name(int t)
{
    return t == T_SCALAR ? "scalar" : t == T_OBJ ? "tobj" : "obj";
}

static void pr_args(const int *a, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] < 0)
            fprintf(stderr, " _");
        else
            fprintf(stderr, " x%d", a[i]);
    }
}

static void pr_code(const Fn *f, const Code *c, int depth);

static void pr_value(const Fn *f, const Code *c)
{
    switch (c->vk) {
    case V_LIT:
        fprintf(stderr, "%d", c->a);
        break;
    case V_CTOR:
        fprintf(stderr, "ctor_%d[%s]", ctors[c->a].tag, ctors[c->a].name);
        pr_args(c->args, c->nargs);
        break;
    case V_PROJ:
        fprintf(stderr, "proj[%d] x%d", c->a, c->y);
        break;
    case V_CALL:
        fprintf(stderr, "%s", fns[c->a].name);
        pr_args(c->args, c->nargs);
        break;
    case V_RESET:
        fprintf(stderr, "reset[%d] x%d", c->a, c->y);
        break;
    case V_REUSE:
        fprintf(stderr, "reuse x%d in ctor_%d[%s]", c->y, ctors[c->a].tag, ctors[c->a].name);
        pr_args(c->args, c->nargs);
        break;
    default:
        fprintf(stderr, "isShared x%d", c->y);
        break;
    }
    (void)f;
}

static void pr_code(const Fn *f, const Code *c, int depth)
{
    for (; c != NULL; c = c->next) {
        fprintf(stderr, "%*s", depth * 2, "");
        switch (c->kind) {
        case K_LET:
            fprintf(stderr, "let x%d : %s := ", c->x, ty_name(f->vty[c->x]));
            pr_value(f, c);
            fprintf(stderr, ";\n");
            break;
        case K_CASE:
            fprintf(stderr, "case x%d of\n", c->x);
            for (int i = 0; i < NALT; i++) {
                fprintf(stderr, "%*s%s =>\n", depth * 2, "", ctors[c->alts[i].ctor].name);
                pr_code(f, c->alts[i].body, depth + 1);
            }
            break;
        case K_RET:
            fprintf(stderr, "ret x%d\n", c->x);
            break;
        case K_INC:
        case K_DEC:
            fprintf(stderr, "%s%s", c->kind == K_INC ? "inc" : "dec", c->flag ? "" : "_ref");
            fprintf(stderr, " x%d;\n", c->x);
            break;
        case K_DEL:
            fprintf(stderr, "del x%d;\n", c->x);
            break;
        case K_OSET:
            fprintf(stderr, "set x%d[%d] := x%d;\n", c->x, c->a, c->y);
            break;
        case K_JP:
            fprintf(stderr, "block_%d (", c->x);
            for (int i = 0; i < c->nargs; i++)
                fprintf(stderr, "%sx%d : %s", i ? ", " : "", c->args[i], ty_name(f->vty[c->args[i]]));
            fprintf(stderr, ") :=\n");
            pr_code(f, c->body, depth + 1);
            break;
        default:
            fprintf(stderr, "jmp block_%d", c->x);
            pr_args(c->args, c->nargs);
            fprintf(stderr, "\n");
            break;
        }
    }
}

static const int *cur_fns; /* the functions of the example, ended by -1 */

static void dump(const char *stage)
{
    fprintf(stderr, "== %s ==\n", stage);
    for (const int *p = cur_fns; *p >= 0; p++) {
        const Fn *f = &fns[*p];
        fprintf(stderr, "def %s", f->name);
        for (int i = 0; i < f->np; i++)
            fprintf(stderr, " (x%d : %s%s)", i, f->borrow[i] ? "@& " : "", ty_name(f->pty[i]));
        fprintf(stderr, " :=\n");
        pr_code(f, f->body, 1);
    }
    fprintf(stderr, "\n");
}

/* ---------------------------------------------------------------------------------------- */
/* The examples                                                                             */
/* ---------------------------------------------------------------------------------------- */

/* The list [1, 2, 3] in variables v to v + 6; the list itself is x(v + 6). */
static Code *list123(int v)
{
    return seq(b_ctor(v, C_NIL, NO), b_lit(v + 1, 3), b_ctor(v + 2, C_CONS, A(v + 1, v)), b_lit(v + 3, 2),
               b_ctor(v + 4, C_CONS, A(v + 3, v + 2)), b_lit(v + 5, 1), b_ctor(v + 6, C_CONS, A(v + 5, v + 4)), NULL);
}

/* The library: every function an example may call. */
static void define_library(void)
{
    Fn *f;

    f = fn_begin(F_ADD, "add", 2, T_SCALAR, T_SCALAR);
    f->ext = 1;
    f = fn_begin(F_LT, "lt", 2, T_SCALAR, T_SCALAR);
    f->ext = 1;

    /* length xs acc: a tail call that only reads the list */
    f = fn_begin(F_LENGTH, "length", 2, T_OBJ, T_SCALAR);
    f->body = b_case(0, C_NIL, b_ret(1), C_CONS,
                     seq(b_proj(2, T_SCALAR, 0, 0), b_proj(3, T_OBJ, 1, 0), b_lit(4, 1), b_call(5, T_SCALAR, F_ADD, A(1, 4)),
                         b_call(6, T_SCALAR, F_LENGTH, A(3, 5)), b_ret(6), NULL));

    /* map_inc xs: rebuilds the list with every element plus one */
    f = fn_begin(F_MAPINC, "map_inc", 1, T_OBJ, 0);
    f->body = b_case(0, C_NIL, seq(b_ctor(1, C_NIL, NO), b_ret(1), NULL), C_CONS,
                     seq(b_proj(2, T_SCALAR, 0, 0), b_proj(3, T_OBJ, 1, 0), b_lit(4, 1), b_call(5, T_SCALAR, F_ADD, A(2, 4)),
                         b_call(6, T_OBJ, F_MAPINC, A(3)), b_ctor(7, C_CONS, A(5, 6)), b_ret(7), NULL));

    /* rev xs acc: reverses onto an accumulator, a tail call that must stay one */
    f = fn_begin(F_REV, "rev", 2, T_OBJ, T_OBJ);
    f->body = b_case(0, C_NIL, b_ret(1), C_CONS,
                     seq(b_proj(2, T_SCALAR, 0, 0), b_proj(3, T_OBJ, 1, 0), b_ctor(4, C_CONS, A(2, 1)),
                         b_call(5, T_OBJ, F_REV, A(3, 4)), b_ret(5), NULL));

    /* append xs ys: returns ys itself when xs is empty */
    f = fn_begin(F_APPEND, "append", 2, T_OBJ, T_OBJ);
    f->body = b_case(0, C_NIL, b_ret(1), C_CONS,
                     seq(b_proj(2, T_SCALAR, 0, 0), b_proj(3, T_OBJ, 1, 0), b_call(4, T_OBJ, F_APPEND, A(3, 1)),
                         b_ctor(5, C_CONS, A(2, 4)), b_ret(5), NULL));

    /* insert t k: binary search tree insertion; node is (left, key, right) */
    f = fn_begin(F_INSERT, "insert", 2, T_OBJ, T_SCALAR);
    f->body = b_case(0, C_LEAF, seq(b_ctor(2, C_LEAF, NO), b_ctor(3, C_LEAF, NO), b_ctor(4, C_NODE, A(2, 1, 3)), b_ret(4), NULL),
                     C_NODE,
                     seq(b_proj(5, T_OBJ, 0, 0), b_proj(6, T_SCALAR, 1, 0), b_proj(7, T_OBJ, 2, 0),
                         b_call(8, T_SCALAR, F_LT, A(1, 6)),
                         b_case(8, C_FALSE, seq(b_call(11, T_OBJ, F_INSERT, A(7, 1)), b_ctor(12, C_NODE, A(5, 6, 11)), b_ret(12), NULL),
                                C_TRUE, seq(b_call(9, T_OBJ, F_INSERT, A(5, 1)), b_ctor(10, C_NODE, A(9, 6, 7)), b_ret(10), NULL)),
                         NULL));

    /* dup x: the same value twice in one constructor */
    f = fn_begin(F_DUP, "dup", 1, T_OBJ, 0);
    f->body = seq(b_ctor(1, C_PAIR, A(0, 0)), b_ret(1), NULL);

    /* headlen xs: the length of xs in front of its tail; xs is still read by length when the
       cell would be reset, so it cannot be reused */
    f = fn_begin(F_HEADLEN, "headlen", 1, T_OBJ, 0);
    f->body = b_case(0, C_NIL, seq(b_ctor(1, C_NIL, NO), b_ret(1), NULL), C_CONS,
                     seq(b_proj(2, T_OBJ, 1, 0), b_lit(3, 0), b_call(4, T_SCALAR, F_LENGTH, A(0, 3)), b_ctor(5, C_CONS, A(4, 2)),
                         b_ret(5), NULL));

    /* filter xs: keeps the elements greater than one; the cell is reused only when one is kept */
    f = fn_begin(F_FILTER, "filter", 1, T_OBJ, 0);
    f->body = b_case(0, C_NIL, seq(b_ctor(1, C_NIL, NO), b_ret(1), NULL), C_CONS,
                     seq(b_proj(2, T_SCALAR, 0, 0), b_lit(4, 1), b_call(5, T_SCALAR, F_LT, A(4, 2)), b_proj(3, T_OBJ, 1, 0),
                         b_case(5, C_FALSE, seq(b_call(6, T_OBJ, F_FILTER, A(3)), b_ret(6), NULL), C_TRUE,
                                seq(b_call(7, T_OBJ, F_FILTER, A(3)), b_ctor(8, C_CONS, A(2, 7)), b_ret(8), NULL)),
                         NULL));

    /* bump xs: a head below two is incremented in place; a head of two leaves the list as it is;
       a larger head is put in front of the whole list. xs itself is read in two of the branches,
       so the reset has to wait for the branch that rebuilds the cell. */
    f = fn_begin(F_BUMP, "bump", 1, T_OBJ, 0);
    f->body = b_case(
        0, C_NIL, seq(b_ctor(1, C_NIL, NO), b_ret(1), NULL), C_CONS,
        seq(b_proj(2, T_SCALAR, 0, 0), b_proj(3, T_OBJ, 1, 0), b_lit(4, 2), b_call(5, T_SCALAR, F_LT, A(2, 4)),
            b_case(5, C_FALSE,
                   seq(b_lit(6, 3), b_call(7, T_SCALAR, F_LT, A(2, 6)),
                       b_case(7, C_FALSE, seq(b_ctor(8, C_CONS, A(2, 0)), b_ret(8), NULL), C_TRUE, b_ret(0)), NULL),
                   C_TRUE, seq(b_lit(9, 1), b_call(10, T_SCALAR, F_ADD, A(2, 9)), b_ctor(11, C_CONS, A(10, 3)), b_ret(11), NULL)),
            NULL));

    /* swap t: exchanges the two children of the root; the key stays where it is */
    f = fn_begin(F_SWAP, "swap", 1, T_OBJ, 0);
    f->body = b_case(0, C_LEAF, seq(b_ctor(1, C_LEAF, NO), b_ret(1), NULL), C_NODE,
                     seq(b_proj(2, T_OBJ, 0, 0), b_proj(3, T_SCALAR, 1, 0), b_proj(4, T_OBJ, 2, 0),
                         b_ctor(5, C_NODE, A(4, 3, 2)), b_ret(5), NULL));

    /* uncons xs: the head and the tail as a pair; a pair is not a list, so its cell cannot reuse
       the cell of xs */
    f = fn_begin(F_UNCONS, "uncons", 1, T_OBJ, 0);
    f->body = b_case(0, C_NIL, seq(b_lit(1, 0), b_ctor(2, C_NIL, NO), b_ctor(3, C_PAIR, A(1, 2)), b_ret(3), NULL), C_CONS,
                     seq(b_proj(4, T_SCALAR, 0, 0), b_proj(5, T_OBJ, 1, 0), b_ctor(6, C_PAIR, A(4, 5)), b_ret(6), NULL));

    /* zipadd xs ys: the sums of the elements, as long as the shorter list; the projections of xs
       sit between the projections of ys and the reset of ys */
    f = fn_begin(F_ZIPADD, "zipadd", 2, T_OBJ, T_OBJ);
    f->body = b_case(
        0, C_NIL, seq(b_ctor(2, C_NIL, NO), b_ret(2), NULL), C_CONS,
        b_case(1, C_NIL, seq(b_ctor(5, C_NIL, NO), b_ret(5), NULL), C_CONS,
               seq(b_proj(6, T_SCALAR, 0, 1), b_proj(3, T_SCALAR, 0, 0), b_proj(7, T_OBJ, 1, 1), b_proj(4, T_OBJ, 1, 0),
                   b_call(8, T_SCALAR, F_ADD, A(3, 6)), b_call(9, T_OBJ, F_ZIPADD, A(4, 7)), b_ctor(10, C_CONS, A(8, 9)),
                   b_ret(10), NULL)));

    /* taillen xs: the length of the mapped tail, alone in a list. The tail is owned by map_inc,
       and the mapped list is released after length, between the reset and the reuse. */
    f = fn_begin(F_TAILLEN, "taillen", 1, T_OBJ, 0);
    f->body = b_case(0, C_NIL, seq(b_ctor(1, C_NIL, NO), b_ret(1), NULL), C_CONS,
                     seq(b_proj(2, T_OBJ, 1, 0), b_call(3, T_OBJ, F_MAPINC, A(2)), b_lit(4, 0),
                         b_call(5, T_SCALAR, F_LENGTH, A(3, 4)), b_ctor(6, C_NIL, NO), b_ctor(7, C_CONS, A(5, 6)), b_ret(7), NULL));
}

static void main_length(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_lit(7, 0), b_call(8, T_SCALAR, F_LENGTH, A(6, 7)), b_ret(8), NULL);
}

static void main_map_inc(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_MAPINC, A(6)), b_ret(7), NULL);
}

static void main_map_shared(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body =
        seq(list123(0), b_call(7, T_OBJ, F_MAPINC, A(6)), b_ctor(8, C_PAIR, A(7, 6)), b_ret(8), NULL);
}

static void main_reverse(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body =
        seq(list123(0), b_ctor(7, C_NIL, NO), b_call(8, T_OBJ, F_REV, A(6, 7)), b_ret(8), NULL);
}

static void main_append(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body =
        seq(list123(0), b_ctor(7, C_NIL, NO), b_lit(8, 5), b_ctor(9, C_CONS, A(8, 7)), b_lit(10, 4), b_ctor(11, C_CONS, A(10, 9)),
            b_call(12, T_OBJ, F_APPEND, A(6, 11)), b_ret(12), NULL);
}

static void main_insert(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body =
        seq(b_ctor(0, C_LEAF, NO), b_lit(1, 2), b_call(2, T_OBJ, F_INSERT, A(0, 1)), b_lit(3, 1), b_call(4, T_OBJ, F_INSERT, A(2, 3)),
            b_lit(5, 3), b_call(6, T_OBJ, F_INSERT, A(4, 5)), b_ret(6), NULL);
}

static void main_dup(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_DUP, A(6)), b_ret(7), NULL);
}

static void main_headlen(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_HEADLEN, A(6)), b_ret(7), NULL);
}

static void main_filter(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_FILTER, A(6)), b_ret(7), NULL);
}

static void main_bump(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_BUMP, A(6)), b_ret(7), NULL);
}

static void main_discard(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_MAPINC, A(6)), b_ret(6), NULL);
}

static void main_self_append(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_APPEND, A(6, 6)), b_ret(7), NULL);
}

static void main_borrow_live(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_lit(7, 0), b_call(8, T_SCALAR, F_LENGTH, A(6, 7)), b_ctor(9, C_PAIR, A(8, 6)), b_ret(9), NULL);
}

static void main_swap(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body =
        seq(b_ctor(0, C_LEAF, NO), b_lit(1, 2), b_call(2, T_OBJ, F_INSERT, A(0, 1)), b_lit(3, 1), b_call(4, T_OBJ, F_INSERT, A(2, 3)),
            b_lit(5, 3), b_call(6, T_OBJ, F_INSERT, A(4, 5)), b_call(7, T_OBJ, F_SWAP, A(6)), b_ret(7), NULL);
}

static void main_uncons(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_UNCONS, A(6)), b_ret(7), NULL);
}

static void main_zipadd(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), list123(7), b_call(14, T_OBJ, F_ZIPADD, A(6, 13)), b_ret(14), NULL);
}

static void main_taillen(void)
{
    Fn *m = fn_begin(F_MAIN, "main", 0, 0, 0);
    m->body = seq(list123(0), b_call(7, T_OBJ, F_TAILLEN, A(6)), b_ret(7), NULL);
}

struct example {
    const char *name;
    const char *shape; /* how to print the result: n number, L list, T tree, (AB) pair */
    void (*define_main)(void);
    int fns[5];
};

static const struct example examples[] = {
    {"length", "n", main_length, {F_LENGTH, F_MAIN, -1}},
    {"map-inc", "L", main_map_inc, {F_MAPINC, F_MAIN, -1}},
    {"map-shared", "(LL)", main_map_shared, {F_MAPINC, F_MAIN, -1}},
    {"reverse", "L", main_reverse, {F_REV, F_MAIN, -1}},
    {"append", "L", main_append, {F_APPEND, F_MAIN, -1}},
    {"insert", "T", main_insert, {F_INSERT, F_MAIN, -1}},
    {"dup", "(LL)", main_dup, {F_DUP, F_MAIN, -1}},
    {"headlen", "L", main_headlen, {F_LENGTH, F_HEADLEN, F_MAIN, -1}},
    {"filter", "L", main_filter, {F_FILTER, F_MAIN, -1}},
    {"bump", "L", main_bump, {F_BUMP, F_MAIN, -1}},
    {"discard", "L", main_discard, {F_MAPINC, F_MAIN, -1}},
    {"self-append", "L", main_self_append, {F_APPEND, F_MAIN, -1}},
    {"borrow-live", "(nL)", main_borrow_live, {F_LENGTH, F_MAIN, -1}},
    {"swap", "T", main_swap, {F_INSERT, F_SWAP, F_MAIN, -1}},
    {"uncons", "(nL)", main_uncons, {F_UNCONS, F_MAIN, -1}},
    {"zipadd", "L", main_zipadd, {F_ZIPADD, F_MAIN, -1}},
    {"taillen", "L", main_taillen, {F_TAILLEN, F_MAPINC, F_LENGTH, F_MAIN, -1}},
};

static const struct example *cur_ex;

static const struct example *find_example(const char *name)
{
    for (size_t i = 0; i < sizeof examples / sizeof examples[0]; i++)
        if (strcmp(examples[i].name, name) == 0)
            return &examples[i];
    return NULL;
}

/* ---------------------------------------------------------------------------------------- */
/* Helpers shared by the passes                                                             */
/* ---------------------------------------------------------------------------------------- */

static int new_var(Fn *f, int ty)
{
    f->vty[f->nv] = ty;
    return f->nv++;
}

static int arg_has(const Code *c, int x)
{
    for (int i = 0; i < c->nargs; i++)
        if (c->args[i] == x)
            return 1;
    return 0;
}

/* Does this node read x? Only the nodes that exist before reference counting. */
static int reads(const Code *c, int x)
{
    switch (c->kind) {
    case K_LET:
        switch (c->vk) {
        case V_LIT:
            return 0;
        case V_CTOR:
        case V_CALL:
            return arg_has(c, x);
        default:
            return c->y == x;
        }
    default: /* CASE and RET */
        return c->x == x;
    }
}

/* Is x read anywhere in c, including its branches? */
static int occurs(const Code *c, int x)
{
    for (; c != NULL; c = c->next) {
        if (reads(c, x))
            return 1;
        if (c->kind == K_CASE)
            for (int i = 0; i < NALT; i++)
                if (occurs(c->alts[i].body, x))
                    return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* stage_source                                                                             */
/* ---------------------------------------------------------------------------------------- */

static void stage_source(void)
{
    define_library();
    cur_ex->define_main();
    dump("stage_source");
}

/* ---------------------------------------------------------------------------------------- */
/* stage_reset_reuse: the functions R, D and S of the paper                                 */
/* ---------------------------------------------------------------------------------------- */

static Code *insert_d(Fn *f, int x, int ctor, Code *c);

/* A cell is only reused for the same constructor, so its tag never changes. */
static int may_reuse(int c1, int c2)
{
    return c1 == c2;
}

/* Replace the first constructor on each path by a reuse of w; true if there was one. */
static int s_go(int w, int ctor, Code *c)
{
    int changed = 0;
    for (; c != NULL; c = c->next) {
        if (c->kind == K_LET && c->vk == V_CTOR && may_reuse(ctor, c->a)) {
            c->vk = V_REUSE;
            c->y = w;
            return 1;
        }
        if (c->kind == K_CASE)
            for (int i = 0; i < NALT; i++)
                changed |= s_go(w, ctor, c->alts[i].body);
    }
    return changed;
}

/* S: x is dead in c. If c builds a cell of the same shape, reset x in front of it. */
static Code *insert_s(Fn *f, int x, int ctor, Code *c)
{
    int w = f->nv;
    if (!s_go(w, ctor, c))
        return c;
    new_var(f, T_OBJ);
    Code *r = node(K_LET);
    r->x = w;
    r->vk = V_RESET;
    r->a = ctors[ctor].size;
    r->y = x;
    r->next = c;
    return r;
}

enum { U_NONE, U_OTHER, U_OWNED };

/* How does this instruction use x? Passing it to a call hands the call a reference. */
static int classify(const Code *c, int x)
{
    if (c->vk == V_CALL && arg_has(c, x))
        return U_OWNED;
    return reads(c, x) ? U_OTHER : U_NONE;
}

/* Returns true if x is alive in c. The first place after which x is dead and that reads it
   as a non-owned argument is where S looks for a constructor. */
static int d_go(Fn *f, int x, int ctor, Code *c)
{
    if (c->kind == K_CASE) {
        if (!occurs(c, x))
            return 0;
        for (int i = 0; i < NALT; i++)
            c->alts[i].body = insert_d(f, x, ctor, c->alts[i].body);
        return 1;
    }
    if (c->kind == K_RET)
        return reads(c, x);
    if (c->vk == V_CTOR && arg_has(c, x))
        return 1; /* x is stored in a new cell: the memory cannot be recycled */
    if (d_go(f, x, ctor, c->next))
        return 1;
    switch (classify(c, x)) {
    case U_OWNED:
        return 1;
    case U_OTHER:
        c->next = insert_s(f, x, ctor, c->next);
        return 1;
    default:
        return 0;
    }
}

/* D: look for the code after which x is dead. */
static Code *insert_d(Fn *f, int x, int ctor, Code *c)
{
    if (d_go(f, x, ctor, c))
        return c;
    return insert_s(f, x, ctor, c);
}

/* R: every constructor alternative that has fields is a candidate. */
static void insert_r(Fn *f, Code *c)
{
    for (; c != NULL; c = c->next) {
        if (c->kind != K_CASE)
            continue;
        for (int i = 0; i < NALT; i++) {
            Alt *a = &c->alts[i];
            insert_r(f, a->body);
            if (ctors[a->ctor].size > 0)
                a->body = insert_d(f, c->x, a->ctor, a->body);
        }
    }
}

static void stage_reset_reuse(void)
{
    for (const int *p = cur_fns; *p >= 0; p++)
        insert_r(&fns[*p], fns[*p].body);
    dump("stage_reset_reuse");
}

/* ---------------------------------------------------------------------------------------- */
/* stage_borrow: a fixpoint over the whole program, starting with every parameter borrowed  */
/* ---------------------------------------------------------------------------------------- */

static Set owned[NFN]; /* per function: variables that must be owned */
static int modified;

static void own(int fn, int v)
{
    if (!HAS(owned[fn], v)) {
        owned[fn] |= BIT(v);
        modified = 1;
    }
}

/* A parameter packed into a constructor is owned, so that no inc is needed to store it. */
static void own_args_if_param(int fn, const Code *c)
{
    for (int i = 0; i < c->nargs; i++)
        if (c->args[i] < fns[fn].np)
            own(fn, c->args[i]);
}

static void borrow_let(int fn, const Code *c)
{
    switch (c->vk) {
    case V_RESET:
        own(fn, c->x);
        own(fn, c->y);
        break;
    case V_REUSE:
        own(fn, c->x);
        own(fn, c->y);
        own_args_if_param(fn, c);
        break;
    case V_PROJ:
        if (HAS(owned[fn], c->y))
            own(fn, c->x);
        if (HAS(owned[fn], c->x))
            own(fn, c->y);
        break;
    case V_CALL:
        own(fn, c->x);
        for (int i = 0; i < c->nargs; i++)
            if (!fns[c->a].borrow[i] && fns[c->a].pty[i] != T_SCALAR)
                own(fn, c->args[i]);
        break;
    case V_CTOR:
        if (ctors[c->a].size > 0) {
            own(fn, c->x);
            own_args_if_param(fn, c);
        }
        break;
    default:
        break;
    }
}

static void borrow_code(int fn, const Code *c)
{
    if (c->kind == K_CASE) {
        for (int i = 0; i < NALT; i++)
            borrow_code(fn, c->alts[i].body);
        return;
    }
    if (c->kind == K_RET)
        return;
    borrow_code(fn, c->next);
    borrow_let(fn, c);
    /* A self call in tail position keeps being a tail call only if no dec follows it, so an
       owned argument makes its parameter owned. */
    if (c->vk == V_CALL && c->a == fn && c->next->kind == K_RET)
        for (int i = 0; i < c->nargs; i++)
            if (HAS(owned[fn], c->args[i]))
                own(fn, i);
}

static void stage_borrow(void)
{
    for (const int *p = cur_fns; *p >= 0; p++)
        for (int i = 0; i < fns[*p].np; i++)
            fns[*p].borrow[i] = fns[*p].pty[i] == T_OBJ;
    do {
        modified = 0;
        for (const int *p = cur_fns; *p >= 0; p++) {
            Fn *f = &fns[*p];
            borrow_code(*p, f->body);
            for (int i = 0; i < f->np; i++) {
                if (f->borrow[i] && HAS(owned[*p], i)) {
                    f->borrow[i] = 0;
                    modified = 1;
                }
            }
        }
    } while (modified);
    dump("stage_borrow");
}

/* ---------------------------------------------------------------------------------------- */
/* stage_rc: insert inc and dec by a backward liveness walk                                 */
/* ---------------------------------------------------------------------------------------- */

typedef struct {
    Set vars;    /* live and owned by someone: may need a dec */
    Set borrows; /* live because a live parent keeps them alive: never dec'd */
} Live;

static Fn *rf;                /* the function being processed */
static Live live;             /* variables live after the current point */
static Set rc_uborrow;        /* borrowed parameters */
static int rc_poss[MAXV];     /* may be a pointer */
static int rc_def[MAXV];      /* is a pointer */
static int rc_parent[MAXV];   /* the object a projection was read from, or -1 */
static int rc_nchild[MAXV];   /* the projections of an object that are in scope */
static int rc_child[MAXV][MAXV];

static int is_live(int v)
{
    return (int)HAS(live.vars, v);
}

static int is_borrowed(int v)
{
    return (int)HAS(live.borrows, v);
}

/* The projections of v, and theirs, that are not live but are kept alive by what is live. */
static void add_desc(int v, Live *lv, Set snapshot, Set exclude)
{
    for (int i = 0; i < rc_nchild[v]; i++) {
        int ch = rc_child[v][i];
        if (!HAS(snapshot | exclude, ch))
            lv->borrows |= BIT(ch);
        add_desc(ch, lv, snapshot, exclude);
    }
}

static void use_var(int v, Set exclude)
{
    if (is_live(v))
        return;
    live.vars |= BIT(v);
    add_desc(v, &live, live.vars, exclude);
}

static void use_args(const Code *c)
{
    Set all = 0;
    for (int i = 0; i < c->nargs; i++)
        all |= BIT(c->args[i]);
    for (int i = 0; i < c->nargs; i++)
        use_var(c->args[i], all);
}

/* At the end of a function the borrowed parameters, and what they keep alive, are all that
   remains. */
static void set_ret_live(void)
{
    Live lv = {0, 0};
    for (int v = 0; v < rf->nv; v++)
        if (HAS(rc_uborrow, v)) {
            lv.borrows |= BIT(v);
            add_desc(v, &lv, 0, 0);
        }
    Set found = lv.borrows;
    for (int v = 0; v < rf->nv; v++)
        if (HAS(found, v))
            add_desc(v, &lv, 0, 0);
    live = lv;
}

static Code *mk_inc(int v, int n, Code *k)
{
    for (int i = 0; i < n; i++) {
        Code *c = node(K_INC);
        c->x = v;
        c->flag = !rc_def[v];
        c->next = k;
        k = c;
    }
    return k;
}

static Code *mk_dec(int v, Code *k)
{
    Code *c = node(K_DEC);
    c->x = v;
    c->flag = !rc_def[v];
    c->next = k;
    return c;
}

static Code *dec_if_needed(int v, Code *k)
{
    if (rc_poss[v] && !is_borrowed(v) && !is_live(v))
        return mk_dec(v, k);
    return k;
}

static int first_occ(const Code *c, int i)
{
    for (int j = 0; j < i; j++)
        if (c->args[j] == c->args[i])
            return 0;
    return 1;
}

/* Is v also passed in a position that only borrows it? */
static int borrow_param(const Code *c, int v, const int *consume)
{
    for (int i = 0; i < c->nargs; i++)
        if (c->args[i] == v && !consume[i])
            return 1;
    return 0;
}

/* Incs in front of c for the arguments it consumes. A value that is dead afterwards and is
   consumed once passes its own reference. */
static Code *inc_before(const Code *c, const int *consume, Code *k)
{
    for (int i = 0; i < c->nargs; i++) {
        int v = c->args[i];
        if (!rc_poss[v] || !first_occ(c, i))
            continue;
        int consumed = 0;
        for (int j = 0; j < c->nargs; j++)
            consumed += c->args[j] == v && consume[j];
        int n = consumed;
        if (!(is_live(v) || is_borrowed(v) || borrow_param(c, v, consume)))
            n--;
        k = mk_inc(v, n, k);
    }
    return k;
}

/* Decs after a call for the arguments that it only borrowed and nobody needs any more. */
static Code *dec_after_call(const Code *c, const int *consume, Code *k)
{
    for (int i = 0; i < c->nargs; i++) {
        int v = c->args[i];
        if (rc_poss[v] && first_occ(c, i) && borrow_param(c, v, consume) && !is_live(v) && !is_borrowed(v))
            k = mk_dec(v, k);
    }
    return k;
}

static Code *let_rc(Code *c, Code *k)
{
    int consume[MAXARGS];
    int z = c->x;
    k = dec_if_needed(z, k);
    switch (c->vk) {
    case V_CTOR:
    case V_REUSE:
        for (int i = 0; i < c->nargs; i++)
            consume[i] = 1;
        c->next = k;
        k = inc_before(c, consume, c);
        break;
    case V_PROJ:
        k = dec_if_needed(c->y, k);
        if (rc_poss[z] && !is_borrowed(z))
            k = mk_inc(z, 1, k);
        c->next = k;
        k = c;
        break;
    case V_CALL:
        for (int i = 0; i < c->nargs; i++)
            consume[i] = !fns[c->a].borrow[i];
        k = dec_after_call(c, consume, k);
        c->next = k;
        k = inc_before(c, consume, c);
        break;
    default: /* literals and resets */
        c->next = k;
        k = c;
        break;
    }
    switch (c->vk) {
    case V_PROJ:
    case V_RESET:
        use_var(c->y, 0);
        break;
    case V_REUSE:
        use_var(c->y, 0);
        use_args(c);
        break;
    case V_LIT:
        break;
    default:
        use_args(c);
        break;
    }
    live.vars &= ~BIT(z);
    live.borrows &= ~BIT(z);
    return k;
}

static Code *rc_code(Code *c);

/* Variables that are live before the case but not in this alternative are released at its
   start. A variable that was borrowed before the case but is owned in this alternative gets
   its reference there. */
static Code *alt_prolog(const Live *alt, Code *k)
{
    Set decs = 0, incs = 0;
    for (int v = 0; v < rf->nv; v++) {
        if (!HAS(live.vars, v))
            continue;
        if (!HAS(alt->vars, v)) {
            if (rc_poss[v] && !is_borrowed(v))
                decs |= BIT(v);
        } else if (is_borrowed(v) && !HAS(alt->borrows, v)) {
            incs |= BIT(v);
        }
    }
    for (int v = 0; v < rf->nv; v++)
        if (HAS(decs, v))
            k = mk_dec(v, k);
    for (int v = 0; v < rf->nv; v++)
        if (HAS(incs, v))
            k = mk_inc(v, 1, k);
    return k;
}

static Code *rc_case(Code *c)
{
    Live alt_live[NALT];
    Live all = {0, 0};
    int d = c->x;
    int poss = rc_poss[d], def = rc_def[d];
    for (int i = 0; i < NALT; i++) {
        rc_poss[d] = rc_def[d] = ctors[c->alts[i].ctor].size > 0;
        live = (Live){0, 0};
        c->alts[i].body = rc_code(c->alts[i].body);
        alt_live[i] = live;
        all.vars |= live.vars;
        all.borrows |= live.borrows;
    }
    live = all;
    use_var(d, 0);
    for (int i = 0; i < NALT; i++) {
        rc_poss[d] = rc_def[d] = ctors[c->alts[i].ctor].size > 0;
        c->alts[i].body = alt_prolog(&alt_live[i], c->alts[i].body);
    }
    rc_poss[d] = poss;
    rc_def[d] = def;
    return c;
}

static Code *rc_code(Code *c)
{
    if (c->kind == K_CASE)
        return rc_case(c);
    if (c->kind == K_RET) {
        set_ret_live();
        use_var(c->x, 0);
        if (rc_poss[c->x] && is_borrowed(c->x))
            return mk_inc(c->x, 1, c);
        return c;
    }
    int z = c->x;
    rc_poss[z] = rf->vty[z] != T_SCALAR;
    rc_def[z] = rf->vty[z] == T_REF;
    rc_parent[z] = -1;
    rc_nchild[z] = 0;
    if (c->vk == V_PROJ && rc_poss[z]) {
        rc_parent[z] = c->y;
        rc_child[c->y][rc_nchild[c->y]++] = z;
    }
    Code *k = rc_code(c->next);
    k = let_rc(c, k);
    if (rc_parent[z] >= 0)
        rc_nchild[rc_parent[z]]--;
    return k;
}

static void stage_rc(void)
{
    for (const int *p = cur_fns; *p >= 0; p++) {
        Fn *f = &fns[*p];
        rf = f;
        rc_uborrow = 0;
        live = (Live){0, 0};
        for (int i = 0; i < f->np; i++) {
            rc_poss[i] = f->pty[i] == T_OBJ;
            rc_def[i] = 0;
            rc_parent[i] = -1;
            rc_nchild[i] = 0;
            if (f->borrow[i])
                rc_uborrow |= BIT(i);
        }
        Code *k = rc_code(f->body);
        for (int i = 0; i < f->np; i++) {
            k = dec_if_needed(i, k);
            live.vars &= ~BIT(i);
            live.borrows &= ~BIT(i);
        }
        f->body = k;
    }
    dump("stage_rc");
}

/* ---------------------------------------------------------------------------------------- */
/* stage_expand: reset and reuse become an isShared test with a fast and a slow path        */
/* ---------------------------------------------------------------------------------------- */

#define MAXDS 64

static int dp_src[MAXV], dp_idx[MAXV]; /* the projection that defines a variable: object, field */

static void scan_proj(const Code *c)
{
    for (; c != NULL; c = c->next) {
        if (c->kind == K_LET && c->vk == V_PROJ) {
            dp_src[c->x] = c->y;
            dp_idx[c->x] = c->a;
        }
        if (c->kind == K_CASE)
            for (int i = 0; i < NALT; i++)
                scan_proj(c->alts[i].body);
    }
}

/* Storing y in field idx of the cell it was read from changes nothing. */
static int self_set(int orig, int idx, int y)
{
    return dp_src[y] == orig && dp_idx[y] == idx;
}

static Code *attach(Code **ds, int n, Code *tail)
{
    for (int i = n - 1; i >= 0; i--) {
        ds[i]->next = tail;
        tail = ds[i];
    }
    return tail;
}

static Code *mk_jmp(int jp, const int *a, int n)
{
    Code *c = node(K_JMP);
    c->x = jp;
    set_args(c, a, n);
    return c;
}

/*
 * The instructions before the reset read the fields of the cell and inc each of them. When the
 * cell is exclusive it is not freed, so the cell keeps those references and the incs can go.
 * mask[i] names the variable whose inc was removed for field i.
 */
static int erase_proj_incs(Code **ds, int n, int target, int *mask)
{
    Code *keep[MAXDS];
    int nk = 0;
    while (n >= 2) {
        Code *d = ds[n - 1];
        if (d->kind == K_LET && d->vk == V_PROJ && d->y == target) {
            mask[d->a] = d->x; /* a scalar field holds no reference */
            keep[nk++] = d;
            n--;
        } else if (d->kind == K_INC) { /* the inc of the projection before it */
            mask[dp_idx[d->x]] = d->x;
            keep[nk++] = ds[n - 2];
            n -= 2;
        } else {
            break;
        }
    }
    while (nk > 0)
        ds[n++] = keep[--nk];
    return n;
}

static Code *ex_code(Fn *f, Code *c);

/* The reuse of the token: update the cell in place if it was exclusive, else allocate. */
static Code *expand_reuse(Fn *f, Code *c, int orig, int shared)
{
    int jp = f->njp++;
    Code *j = node(K_JP);
    j->x = jp;
    j->nargs = 1;
    j->args[0] = c->x;
    j->body = c->next;

    int fresh = new_var(f, T_REF);
    Code *alloc = node(K_LET);
    alloc->x = fresh;
    alloc->vk = V_CTOR;
    alloc->a = c->a;
    set_args(alloc, c->args, c->nargs);
    alloc->next = mk_jmp(jp, A(fresh));

    Code *fast = mk_jmp(jp, A(c->y));
    for (int i = 0; i < c->nargs; i++) {
        if (self_set(orig, i, c->args[i]))
            continue;
        Code *s = node(K_OSET);
        s->x = c->y;
        s->a = i;
        s->y = c->args[i];
        s->next = fast;
        fast = s;
    }
    j->next = b_case(shared, C_FALSE, fast, C_TRUE, alloc);
    return j;
}

/* Find the reuse of the token in c, and a dec of the token on the paths that do not reuse it. */
static Code *expand_cont(Fn *f, int token, Code *c, int orig, int shared)
{
    if (c->kind == K_DEC && c->x == token) {
        c->kind = K_DEL;
        return c;
    }
    if (c->kind == K_LET && c->vk == V_REUSE)
        return expand_reuse(f, c, orig, shared);
    if (c->kind == K_CASE) {
        for (int i = 0; i < NALT; i++)
            c->alts[i].body = expand_cont(f, token, c->alts[i].body, orig, shared);
        return c;
    }
    c->next = expand_cont(f, token, c->next, orig, shared);
    return c;
}

/* ds holds the instructions of this block before the reset. */
static Code *expand_reset(Fn *f, Code **ds, int n, Code *rst)
{
    int mask[MAXFIELDS] = {-1, -1, -1};
    int orig = rst->y, token = rst->x;
    n = erase_proj_incs(ds, n, orig, mask);

    int shared = new_var(f, T_SCALAR);
    Code *k = expand_cont(f, token, rst->next, orig, shared);
    k = ex_code(f, k);
    int jp = f->njp++;
    Code *resetjp = node(K_JP);
    resetjp->x = jp;
    resetjp->nargs = 2;
    resetjp->args[0] = token;
    resetjp->args[1] = shared;
    resetjp->body = k;

    int chk = new_var(f, T_SCALAR);
    Code *test = node(K_LET);
    test->x = chk;
    test->vk = V_ISSHARED;
    test->y = orig;

    /* The cell is shared: inc what the removed incs would have covered, dec the cell, no token. */
    Code *slow = mk_jmp(jp, A(-1, chk));
    Code *d = node(K_DEC);
    d->x = orig;
    d->flag = 1;
    d->next = slow;
    slow = d;
    for (int i = 0; i < rst->a; i++) {
        if (mask[i] < 0 || f->vty[mask[i]] == T_SCALAR)
            continue;
        Code *inc = node(K_INC);
        inc->x = mask[i];
        inc->flag = 1;
        inc->next = slow;
        slow = inc;
    }

    /* The cell is exclusive: the fields nobody read are released, the cell is the token. */
    Code *fast = mk_jmp(jp, A(orig, chk));
    for (int i = 0; i < rst->a; i++) {
        if (mask[i] >= 0)
            continue;
        int u = new_var(f, T_OBJ);
        Code *pu = node(K_LET);
        pu->x = u;
        pu->vk = V_PROJ;
        pu->a = i;
        pu->y = orig;
        Code *du = node(K_DEC);
        du->x = u;
        du->flag = 1;
        du->next = fast;
        pu->next = du;
        fast = pu;
    }

    test->next = b_case(chk, C_FALSE, fast, C_TRUE, slow);
    resetjp->next = test;
    return attach(ds, n, resetjp);
}

/* Walk a block, remembering the straight-line instructions up to the next reset. */
static Code *ex_code(Fn *f, Code *c)
{
    Code *ds[MAXDS];
    int n = 0;
    for (;;) {
        if (c->kind == K_CASE) {
            for (int i = 0; i < NALT; i++)
                c->alts[i].body = ex_code(f, c->alts[i].body);
            return attach(ds, n, c);
        }
        if (c->kind == K_RET || c->kind == K_JMP)
            return attach(ds, n, c);
        if (c->kind == K_LET && c->vk == V_RESET)
            return expand_reset(f, ds, n, c);
        if (c->kind == K_JP)
            c->body = ex_code(f, c->body);
        ds[n++] = c;
        c = c->next;
    }
}

static void stage_expand(void)
{
    for (const int *p = cur_fns; *p >= 0; p++) {
        Fn *f = &fns[*p];
        memset(dp_src, -1, sizeof dp_src);
        scan_proj(f->body);
        f->body = ex_code(f, f->body);
    }
    dump("stage_expand");
}

/* ---------------------------------------------------------------------------------------- */
/* stage_run: an interpreter on a heap that counts references and checks them               */
/* ---------------------------------------------------------------------------------------- */

typedef int64_t Val;
#define BOX(n) ((Val)(((uint64_t)(n) << 1) | 1))
#define IS_SCALAR(v) ((int)((v)&1))
#define UNBOX(v) ((long)((v) >> 1))

struct obj {
    int rc, tag, size, live;
    Val f[MAXFIELDS];
};

static struct obj heap[HEAP]; /* a reference is an index into the heap, shifted left by one */
static int nheap;
static struct {
    long allocs, frees, incs, decs, reused, fresh;
    int live;
} st;

/* GCOVR_EXCL_START: the checks below fire only if a pass is wrong. */
static void die(const char *msg)
{
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}
/* GCOVR_EXCL_STOP */

static struct obj *obj_of(Val v)
{
    int idx = (int)(v >> 1);
    if (idx <= 0 || idx > nheap || !heap[idx].live) // GCOVR_EXCL_BR_LINE: use after free
        die("use of a freed object");                // GCOVR_EXCL_LINE: see above
    return &heap[idx];
}

static Val alloc(int tag, int size, const Val *fields)
{
    struct obj *o = &heap[++nheap];
    o->rc = 1;
    o->tag = tag;
    o->size = size;
    o->live = 1;
    for (int i = 0; i < size; i++)
        o->f[i] = fields[i];
    st.allocs++;
    st.live++;
    return (Val)nheap << 1;
}

static void release(struct obj *o)
{
    o->live = 0;
    st.frees++;
    st.live--;
}

static void dec(Val v)
{
    struct obj *o = obj_of(v);
    if (o->rc > 1) {
        o->rc--;
        return;
    }
    for (int i = 0; i < o->size; i++)
        if (!IS_SCALAR(o->f[i]))
            dec(o->f[i]);
    release(o);
}

static Val builtin(int fn, const Val *a)
{
    return fn == F_ADD ? BOX(UNBOX(a[0]) + UNBOX(a[1])) : BOX(UNBOX(a[0]) < UNBOX(a[1]));
}

static Val run(int fi, const Val *argv);

static Val eval_let(const Code *c, const Val *env)
{
    Val a[MAXARGS];
    for (int i = 0; i < c->nargs; i++)
        a[i] = env[c->args[i]];
    switch (c->vk) {
    case V_LIT:
        return BOX(c->a);
    case V_CTOR:
        if (ctors[c->a].size == 0)
            return BOX(ctors[c->a].tag);
        return alloc(ctors[c->a].tag, ctors[c->a].size, a);
    case V_PROJ:
        return obj_of(env[c->y])->f[c->a];
    case V_ISSHARED:
        if (obj_of(env[c->y])->rc == 1)
            st.reused++;
        else
            st.fresh++;
        return BOX(obj_of(env[c->y])->rc != 1);
    default: /* V_CALL */
        return fns[c->a].ext ? builtin(c->a, a) : run(c->a, a);
    }
}

static Val run(int fi, const Val *argv)
{
    const Fn *f = &fns[fi];
    const Code *jps[MAXJP];
    Val env[MAXV];
    for (int i = 0; i < f->np; i++)
        env[i] = argv[i];
    for (const Code *c = f->body;;) {
        switch (c->kind) {
        case K_LET:
            env[c->x] = eval_let(c, env);
            c = c->next;
            break;
        case K_INC:
            if (IS_SCALAR(env[c->x])) {
                if (!c->flag) // GCOVR_EXCL_BR_LINE: inc_ref says the value is a pointer
                    die("inc_ref of a scalar"); // GCOVR_EXCL_LINE: see above
            } else {
                obj_of(env[c->x])->rc++;
                st.incs++;
            }
            c = c->next;
            break;
        case K_DEC:
            if (IS_SCALAR(env[c->x])) {
                if (!c->flag) // GCOVR_EXCL_BR_LINE: dec_ref says the value is a pointer
                    die("dec_ref of a scalar"); // GCOVR_EXCL_LINE: see above
            } else {
                st.decs++;
                dec(env[c->x]);
            }
            c = c->next;
            break;
        case K_DEL:
            release(obj_of(env[c->x]));
            c = c->next;
            break;
        case K_OSET:
            obj_of(env[c->x])->f[c->a] = env[c->y];
            c = c->next;
            break;
        case K_JP:
            jps[c->x] = c;
            c = c->next;
            break;
        case K_JMP: {
            const Code *j = jps[c->x];
            Val t[MAXARGS];
            for (int i = 0; i < c->nargs; i++)
                t[i] = c->args[i] < 0 ? BOX(0) : env[c->args[i]];
            for (int i = 0; i < c->nargs; i++)
                env[j->args[i]] = t[i];
            c = j->body;
            break;
        }
        case K_CASE: {
            Val v = env[c->x];
            int tag = IS_SCALAR(v) ? (int)UNBOX(v) : obj_of(v)->tag;
            c = ctors[c->alts[0].ctor].tag == tag ? c->alts[0].body : c->alts[1].body;
            break;
        }
        default: /* K_RET */
            return env[c->x];
        }
    }
}

/* Print v as the shape says; returns the rest of the shape. */
static const char *show(Val v, const char *s)
{
    if (*s == 'n') {
        printf("%ld", UNBOX(v));
    } else if (*s == 'L') {
        printf("[");
        for (int first = 1; !IS_SCALAR(v); first = 0, v = obj_of(v)->f[1])
            printf("%s%ld", first ? "" : ",", UNBOX(obj_of(v)->f[0]));
        printf("]");
    } else if (*s == 'T') {
        if (IS_SCALAR(v)) {
            printf(".");
        } else {
            struct obj *o = obj_of(v);
            printf("(");
            show(o->f[0], "T");
            printf(" %ld ", UNBOX(o->f[1]));
            show(o->f[2], "T");
            printf(")");
        }
    } else {
        struct obj *o = obj_of(v);
        printf("(");
        s = show(o->f[0], s + 1);
        printf(", ");
        s = show(o->f[1], s);
        printf(")");
        return s + 1;
    }
    return s + 1;
}

static void stage_run(void)
{
    Val r = run(F_MAIN, NULL);
    show(r, cur_ex->shape);
    printf("\n");
    if (!IS_SCALAR(r))
        dec(r);
    if (st.live != 0) // GCOVR_EXCL_BR_LINE: a leak
        die("objects leaked"); // GCOVR_EXCL_LINE: see above
    fprintf(stderr, "== stage_run ==\n");
    fprintf(stderr, "allocations %ld, frees %ld, incs %ld, decs %ld\n", st.allocs, st.frees, st.incs, st.decs);
    fprintf(stderr, "reset found the cell exclusive %ld times and shared %ld times\n", st.reused, st.fresh);
    fprintf(stderr, "objects alive at exit 0\n");
}

int main(int argc, char **argv)
{
    if (argc < 2 || (cur_ex = find_example(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    cur_fns = cur_ex->fns;
    stage_source();
    stage_reset_reuse();
    stage_borrow();
    stage_rc();
    stage_expand();
    stage_run();
    return 0;
}
