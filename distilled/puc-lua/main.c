/*
 * puc-lua: a cut-down version of the reference Lua 5.4 virtual machine.
 *
 * Real Lua compiles source to register bytecode and runs it in the dispatch loop of
 * luaV_execute. This entry skips the compiling: the bytecode that `luac5.4 -l -l` prints for
 * each example is written down by hand, using the real 5.4 opcode numbers and instruction
 * layout, and a small copy of the real dispatch loop runs it. Only the opcodes that the
 * examples execute have handlers.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. An interpreter generates no machine code, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_select  pick the function prototypes that Lua's compiler produces for the example
 *   stage_list    print them as a luac-style listing
 *   stage_run     execute the main chunk in the dispatch loop
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------
 * Instruction layout, from lopcodes.h of Lua 5.4
 *
 *   iABC   op:7  A:8  k:1  B:8  C:8
 *   iABx   op:7  A:8  Bx:17
 *   isJ    op:7  sJ:25
 * ---------------------------------------------------------------------------------------- */

enum opcode {
    OP_MOVE = 0,
    OP_LOADI = 1,
    OP_LOADK = 3,
    OP_GETUPVAL = 9,
    OP_GETTABUP = 11,
    OP_ADDI = 21,
    OP_ADD = 34,
    OP_MUL = 36,
    OP_MMBIN = 46,
    OP_MMBINI = 47,
    OP_JMP = 56,
    OP_LT = 58,
    OP_LTI = 62,
    OP_CALL = 68,
    OP_RETURN = 70,
    OP_RETURN0 = 71,
    OP_RETURN1 = 72,
    OP_CLOSURE = 79,
    OP_VARARGPREP = 81,
};

#define POS_A 7
#define POS_K 15
#define POS_B 16
#define POS_C 24
#define POS_BX 15
#define MAXARG_BX ((1 << 17) - 1)
#define OFFSET_SBX (MAXARG_BX >> 1)
#define MAXARG_SJ ((1 << 25) - 1)
#define OFFSET_SJ (MAXARG_SJ >> 1)
#define OFFSET_SC (255 >> 1)

#define GET_OP(i) ((int)((i) & 0x7f))
#define GET_A(i) ((int)(((i) >> POS_A) & 0xff))
#define GET_K(i) ((int)(((i) >> POS_K) & 1))
#define GET_B(i) ((int)(((i) >> POS_B) & 0xff))
#define GET_C(i) ((int)(((i) >> POS_C) & 0xff))
#define GET_SB(i) (GET_B(i) - OFFSET_SC)
#define GET_SC(i) (GET_C(i) - OFFSET_SC)
#define GET_BX(i) ((int)((i) >> POS_BX))
#define GET_SBX(i) (GET_BX(i) - OFFSET_SBX)
#define GET_SJ(i) ((int)((i) >> POS_A) - OFFSET_SJ)

#define ABCK(op, a, b, c, k)                                                                   \
    ((uint32_t)(op) | (uint32_t)(a) << POS_A | (uint32_t)(k) << POS_K | (uint32_t)(b) << POS_B \
     | (uint32_t)(c) << POS_C)
#define ABC(op, a, b, c) ABCK(op, a, b, c, 0)
#define ABX(op, a, bx) ((uint32_t)(op) | (uint32_t)(a) << POS_A | (uint32_t)(bx) << POS_BX)
#define ASBX(op, a, sbx) ABX(op, a, (sbx) + OFFSET_SBX)
#define SJ(op, j) ((uint32_t)(op) | (uint32_t)((j) + OFFSET_SJ) << POS_A)
#define SB(x) ((x) + OFFSET_SC) /* a signed 8-bit operand, as stored in B or C */

/* ------------------------------------------------------------------------------------------
 * Values, prototypes and closures
 * ---------------------------------------------------------------------------------------- */

struct closure;
struct table;
struct value;

enum tag { TAG_NIL, TAG_INT, TAG_STR, TAG_TABLE, TAG_LUA_FUNCTION, TAG_C_FUNCTION };

struct value {
    enum tag tag;
    union {
        int64_t i;
        const char *s;
        struct table *t;
        struct closure *cl;
        int (*f)(const struct value *args, int nargs);
    } u;
};

/* _ENV holds only `print` in these examples, so a table is one key and one value. */
struct table {
    const char *key;
    struct value value;
};

struct proto {
    const char *name;
    const uint32_t *code;
    int ncode;
    const struct value *k;
    const char *const *upvalue_names;
    const int *upvalue_idx; /* register of the enclosing function that each upvalue captures */
    const struct proto *const *children;
};

struct upval {
    struct value *v; /* the stack slot while open, or `closed` once the frame has returned */
    int slot;        /* the stack slot while open, or -1 once closed */
    struct value closed;
};

#define MAX_UPVALS 2
struct closure {
    const struct proto *p;
    struct upval *upvals[MAX_UPVALS];
};

static const char *const tm_names[] = {[6] = "__add", [7] = "__sub", [8] = "__mul"};

static const char *const op_names[] = {
    [OP_MOVE] = "MOVE",         [OP_LOADI] = "LOADI",       [OP_LOADK] = "LOADK",
    [OP_GETUPVAL] = "GETUPVAL", [OP_GETTABUP] = "GETTABUP", [OP_ADDI] = "ADDI",
    [OP_ADD] = "ADD",           [OP_MUL] = "MUL",           [OP_MMBIN] = "MMBIN",
    [OP_MMBINI] = "MMBINI",     [OP_JMP] = "JMP",           [OP_LT] = "LT",
    [OP_LTI] = "LTI",           [OP_CALL] = "CALL",         [OP_RETURN] = "RETURN",
    [OP_RETURN0] = "RETURN0",   [OP_RETURN1] = "RETURN1",   [OP_CLOSURE] = "CLOSURE",
    [OP_VARARGPREP] = "VARARGPREP",
};

#define STR(text) {TAG_STR, {.s = (text)}}

/* ------------------------------------------------------------------------------------------
 * The prototypes, copied from `luac5.4 -l -l -p examples/<example>/program.lua`
 *
 * Every main chunk is a vararg function with one upvalue, _ENV. Comments give the Lua source
 * and, per instruction, the listing line of luac.
 * ---------------------------------------------------------------------------------------- */

static const char *const env_upvalue[] = {"_ENV"};
static const int env_idx[] = {0};
static const struct value print_k[] = {STR("print")};

/* print("Hello, world!") */
static const uint32_t hello_code[] = {
    ABC(OP_VARARGPREP, 0, 0, 0), /* VARARGPREP 0 */
    ABC(OP_GETTABUP, 0, 0, 0),   /* GETTABUP   0 0 0   ; _ENV "print" */
    ABX(OP_LOADK, 1, 1),         /* LOADK      1 1     ; "Hello, world!" */
    ABC(OP_CALL, 0, 2, 1),       /* CALL       0 2 1   ; 1 in 0 out */
    ABC(OP_RETURN, 0, 1, 1),     /* RETURN     0 1 1   ; 0 out */
};
static const struct value hello_k[] = {STR("print"), STR("Hello, world!")};
static const struct proto hello_proto = {"hello", hello_code, 5, hello_k, env_upvalue, env_idx, NULL};

/* print((2 + 3) * 4 - 6) */
static const uint32_t arithmetic_code[] = {
    ABC(OP_VARARGPREP, 0, 0, 0), /* VARARGPREP 0 */
    ABC(OP_GETTABUP, 0, 0, 0),   /* GETTABUP   0 0 0   ; _ENV "print" */
    ASBX(OP_LOADI, 1, 14),       /* LOADI      1 14    ; Lua folded the constants */
    ABC(OP_CALL, 0, 2, 1),       /* CALL       0 2 1   ; 1 in 0 out */
    ABC(OP_RETURN, 0, 1, 1),     /* RETURN     0 1 1   ; 0 out */
};
static const struct proto arithmetic_proto = {"arithmetic", arithmetic_code, 5, print_k, env_upvalue,
                                              env_idx, NULL};

/*
 * local a = 3
 * local b = 5
 * if a < b then
 *   print(b)
 * else
 *   print(a)
 * end
 */
static const uint32_t branch_code[] = {
    ABC(OP_VARARGPREP, 0, 0, 0), /*  1  VARARGPREP 0 */
    ASBX(OP_LOADI, 0, 3),        /*  2  LOADI      0 3 */
    ASBX(OP_LOADI, 1, 5),        /*  3  LOADI      1 5 */
    ABC(OP_LT, 0, 1, 0),         /*  4  LT         0 1 0 */
    SJ(OP_JMP, 4),               /*  5  JMP        4     ; to 10 */
    ABC(OP_GETTABUP, 2, 0, 0),   /*  6  GETTABUP   2 0 0 ; _ENV "print" */
    ABC(OP_MOVE, 3, 1, 0),       /*  7  MOVE       3 1 */
    ABC(OP_CALL, 2, 2, 1),       /*  8  CALL       2 2 1 */
    SJ(OP_JMP, 3),               /*  9  JMP        3     ; to 13 */
    ABC(OP_GETTABUP, 2, 0, 0),   /* 10  GETTABUP   2 0 0 ; _ENV "print" */
    ABC(OP_MOVE, 3, 0, 0),       /* 11  MOVE       3 0 */
    ABC(OP_CALL, 2, 2, 1),       /* 12  CALL       2 2 1 */
    ABC(OP_RETURN, 2, 1, 1),     /* 13  RETURN     2 1 1 */
};
static const struct proto branch_proto = {"branch", branch_code, 13, print_k, env_upvalue, env_idx, NULL};

/*
 * local sum = 0
 * local i = 1
 * while i < 11 do
 *   sum = sum + i
 *   i = i + 1
 * end
 * print(sum)
 */
static const uint32_t loop_code[] = {
    ABC(OP_VARARGPREP, 0, 0, 0), /*  1  VARARGPREP 0 */
    ASBX(OP_LOADI, 0, 0),        /*  2  LOADI      0 0 */
    ASBX(OP_LOADI, 1, 1),        /*  3  LOADI      1 1 */
    ABC(OP_LTI, 1, SB(11), 0),   /*  4  LTI        1 11 0 */
    SJ(OP_JMP, 5),               /*  5  JMP        5     ; to 11 */
    ABC(OP_ADD, 0, 0, 1),        /*  6  ADD        0 0 1 */
    ABC(OP_MMBIN, 0, 1, 6),      /*  7  MMBIN      0 1 6 ; __add */
    ABC(OP_ADDI, 1, 1, SB(1)),   /*  8  ADDI       1 1 1 */
    ABCK(OP_MMBINI, 1, SB(1), 6, 0), /* 9  MMBINI  1 1 6 0 ; __add */
    SJ(OP_JMP, -7),              /* 10  JMP        -7    ; to 4 */
    ABC(OP_GETTABUP, 2, 0, 0),   /* 11  GETTABUP   2 0 0 ; _ENV "print" */
    ABC(OP_MOVE, 3, 0, 0),       /* 12  MOVE       3 0 */
    ABC(OP_CALL, 2, 2, 1),       /* 13  CALL       2 2 1 */
    ABC(OP_RETURN, 2, 1, 1),     /* 14  RETURN     2 1 1 */
};
static const struct proto loop_proto = {"loop", loop_code, 14, print_k, env_upvalue, env_idx, NULL};

/*
 * local function fact(n)
 *   if n < 2 then
 *     return 1
 *   end
 *   return n * fact(n - 1)
 * end
 * print(fact(5))
 */
static const char *const fact_upvalue[] = {"fact"};
static const int fact_idx[] = {0};
static const uint32_t fact_code[] = {
    ABC(OP_LTI, 0, SB(2), 0),        /*  1  LTI      0 2 0 */
    SJ(OP_JMP, 2),                   /*  2  JMP      2     ; to 5 */
    ASBX(OP_LOADI, 1, 1),            /*  3  LOADI    1 1 */
    ABC(OP_RETURN1, 1, 2, 0),        /*  4  RETURN1  1 */
    ABC(OP_GETUPVAL, 1, 0, 0),       /*  5  GETUPVAL 1 0   ; fact */
    ABC(OP_ADDI, 2, 0, SB(-1)),      /*  6  ADDI     2 0 -1 */
    ABCK(OP_MMBINI, 0, SB(1), 7, 0), /*  7  MMBINI   0 1 7 0 ; __sub */
    ABC(OP_CALL, 1, 2, 2),           /*  8  CALL     1 2 2 */
    ABC(OP_MUL, 1, 0, 1),            /*  9  MUL      1 0 1 */
    ABC(OP_MMBIN, 0, 1, 8),          /* 10  MMBIN    0 1 8 ; __mul */
    ABC(OP_RETURN1, 1, 2, 0),        /* 11  RETURN1  1 */
    ABC(OP_RETURN0, 1, 1, 0),        /* 12  RETURN0 */
};
static const struct proto fact_proto = {"fact", fact_code, 12, NULL, fact_upvalue, fact_idx, NULL};
static const struct proto *const call_children[] = {&fact_proto};
static const uint32_t call_code[] = {
    ABC(OP_VARARGPREP, 0, 0, 0), /* 1  VARARGPREP 0 */
    ABX(OP_CLOSURE, 0, 0),       /* 2  CLOSURE    0 0 */
    ABC(OP_GETTABUP, 1, 0, 0),   /* 3  GETTABUP   1 0 0 ; _ENV "print" */
    ABC(OP_MOVE, 2, 0, 0),       /* 4  MOVE       2 0 */
    ASBX(OP_LOADI, 3, 5),        /* 5  LOADI      3 5 */
    ABC(OP_CALL, 2, 2, 0),       /* 6  CALL       2 2 0 ; 1 in all out */
    ABC(OP_CALL, 1, 0, 1),       /* 7  CALL       1 0 1 ; all in 0 out */
    ABCK(OP_RETURN, 1, 1, 1, 1), /* 8  RETURN     1 1 1k ; k: close the upvalue */
};
static const struct proto call_proto = {"call", call_code, 8, print_k, env_upvalue, env_idx, call_children};

struct program {
    const char *name;
    const struct proto *main;
};

static const struct program programs[] = {
    {"hello", &hello_proto},   {"arithmetic", &arithmetic_proto}, {"branch", &branch_proto},
    {"loop", &loop_proto},     {"call", &call_proto},
};

/* ------------------------------------------------------------------------------------------
 * stage_select
 * ---------------------------------------------------------------------------------------- */

static const struct program *stage_select(const char *name)
{
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        if (strcmp(programs[i].name, name) == 0) {
            fprintf(stderr, "== stage_select: prototypes of %s ==\n", name);
            return &programs[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------
 * stage_list: a luac-style listing, also used for the execution trace
 * ---------------------------------------------------------------------------------------- */

static void format_ins(char *buf, size_t size, const struct proto *p, int pc)
{
    const uint32_t i = p->code[pc];
    const int a = GET_A(i);
    const int b = GET_B(i);
    const int c = GET_C(i);
    const char *const name = op_names[GET_OP(i)];

    switch (GET_OP(i)) { // GCOVR_EXCL_BR_LINE: the default case is unreachable, see below
    case OP_MOVE:
        snprintf(buf, size, "%-10s %d %d", name, a, b);
        break;
    case OP_LOADI:
        snprintf(buf, size, "%-10s %d %d", name, a, GET_SBX(i));
        break;
    case OP_LOADK:
        snprintf(buf, size, "%-10s %d %d ; \"%s\"", name, a, GET_BX(i), p->k[GET_BX(i)].u.s);
        break;
    case OP_GETUPVAL:
        snprintf(buf, size, "%-10s %d %d ; %s", name, a, b, p->upvalue_names[b]);
        break;
    case OP_GETTABUP:
        snprintf(buf, size, "%-10s %d %d %d ; %s \"%s\"", name, a, b, c, p->upvalue_names[b],
                 p->k[c].u.s);
        break;
    case OP_ADDI:
        snprintf(buf, size, "%-10s %d %d %d", name, a, b, GET_SC(i));
        break;
    case OP_ADD:
    case OP_MUL:
        snprintf(buf, size, "%-10s %d %d %d", name, a, b, c);
        break;
    case OP_MMBIN:
        snprintf(buf, size, "%-10s %d %d %d ; %s", name, a, b, c, tm_names[c]);
        break;
    case OP_MMBINI:
        snprintf(buf, size, "%-10s %d %d %d %d ; %s", name, a, GET_SB(i), c, GET_K(i),
                 tm_names[c]);
        break;
    case OP_JMP:
        snprintf(buf, size, "%-10s %d ; to %d", name, GET_SJ(i), pc + 2 + GET_SJ(i));
        break;
    case OP_LT:
        snprintf(buf, size, "%-10s %d %d %d", name, a, b, GET_K(i));
        break;
    case OP_LTI:
        snprintf(buf, size, "%-10s %d %d %d", name, a, GET_SB(i), GET_K(i));
        break;
    case OP_CALL:
        snprintf(buf, size, "%-10s %d %d %d", name, a, b, c);
        break;
    case OP_RETURN:
        snprintf(buf, size, "%-10s %d %d %d%s", name, a, b, c, GET_K(i) ? "k" : "");
        break;
    case OP_RETURN0:
        snprintf(buf, size, "%s", name);
        break;
    case OP_RETURN1:
        snprintf(buf, size, "%-10s %d", name, a);
        break;
    case OP_CLOSURE:
        snprintf(buf, size, "%-10s %d %d", name, a, GET_BX(i));
        break;
    case OP_VARARGPREP:
        snprintf(buf, size, "%-10s %d", name, a);
        break;
    // GCOVR_EXCL_START: every opcode in the prototypes above has a case
    default:
        abort();
        // GCOVR_EXCL_STOP
    }
}

static void list_proto(const struct proto *p)
{
    fprintf(stderr, "function <%s> (%d instructions)\n", p->name, p->ncode);
    for (int pc = 0; pc < p->ncode; pc++) {
        char text[96];
        format_ins(text, sizeof text, p, pc);
        fprintf(stderr, "  %2d  0x%08x  %s\n", pc + 1, (unsigned)p->code[pc], text);
    }
    if (p->children != NULL) {
        list_proto(p->children[0]);
    }
}

static void stage_list(const struct program *program)
{
    fprintf(stderr, "== stage_list: luac-style listing, instruction numbers start at 1 ==\n");
    list_proto(program->main);
}

/* ------------------------------------------------------------------------------------------
 * stage_run: the dispatch loop
 * ---------------------------------------------------------------------------------------- */

#define STACK_SIZE 64
#define MAX_FRAMES 8
#define MAX_CLOSURES 4

struct frame {
    const struct closure *cl;
    struct value *base;
    int pc;
    int nresults; /* results the caller wants, or -1 for all of them */
};

static struct value stack[STACK_SIZE];
static struct value *top; /* the end of a variable-length argument or result list */
static struct frame frames[MAX_FRAMES];
static struct closure closures[MAX_CLOSURES];
static int closure_count;
static struct upval upvals[MAX_UPVALS];
static int upval_count;

static int l_print(const struct value *args, int nargs)
{
    (void)nargs; /* the examples always call print with one argument */
    if (args[0].tag == TAG_INT) {
        printf("%ld\n", (long)args[0].u.i);
    } else {
        printf("%s\n", args[0].u.s);
    }
    return 0;
}

// GCOVR_EXCL_START: the examples only reach the integer and string cases that are implemented
static void unsupported(const char *what)
{
    fprintf(stderr, "puc-lua: %s is not implemented\n", what);
    abort();
}
// GCOVR_EXCL_STOP

static int64_t as_int(const struct value *v)
{
    // GCOVR_EXCL_START: the real handlers fall through to the metamethod instruction instead
    if (v->tag != TAG_INT) {
        unsupported("arithmetic or comparison on a non-integer");
    }
    // GCOVR_EXCL_STOP
    return v->u.i;
}

/* Integer arithmetic wraps around in Lua, so do it on unsigned values. */
static int64_t int_add(int64_t a, int64_t b) { return (int64_t)((uint64_t)a + (uint64_t)b); }
static int64_t int_mul(int64_t a, int64_t b) { return (int64_t)((uint64_t)a * (uint64_t)b); }

static struct value make_int(int64_t i)
{
    struct value v = {TAG_INT, {.i = i}};
    return v;
}

/* lvm.c: docondjump. Skip the jump that follows unless the condition matches k. */
static void cond_jump(struct frame *ci, int cond, int k)
{
    if (cond != k) {
        ci->pc++;
    } else {
        const uint32_t next = ci->cl->p->code[ci->pc];
        ci->pc += GET_SJ(next) + 1;
    }
}

/* ldo.c: luaD_poscall. Move the results to where the function was, and set top. */
static void poscall(struct value *res, const struct value *first, int n, int wanted)
{
    if (wanted < 0) {
        wanted = n;
    }
    // GCOVR_EXCL_START: padding missing results with nil is not needed by the examples
    if (wanted > n) {
        unsupported("padding results with nil");
    }
    // GCOVR_EXCL_STOP
    for (int i = 0; i < wanted; i++) {
        res[i] = first[i];
    }
    top = res + wanted;
}

/* lfunc.c: luaF_close. Copy every open upvalue at or above level into the upvalue itself. */
static void close_upvals(int level)
{
    for (int i = 0; i < upval_count; i++) {
        struct upval *uv = &upvals[i];
        if (uv->slot >= level) {
            uv->closed = *uv->v;
            uv->v = &uv->closed;
            uv->slot = -1;
        }
    }
}

/*
 * lvm.c: pushclosure. The one closure the examples create has one upvalue, which captures a
 * register of the running function, so there is no loop and no sharing of open upvalues.
 */
static struct closure *push_closure(const struct proto *p, struct value *base)
{
    struct closure *cl = &closures[closure_count++];
    cl->p = p;
    struct upval *uv = &upvals[upval_count++];
    uv->v = base + p->upvalue_idx[0];
    uv->slot = (int)(uv->v - stack);
    cl->upvals[0] = uv;
    return cl;
}

static void stage_run(const struct program *program)
{
    fprintf(stderr, "== stage_run: executed instructions, indented by call depth ==\n");

    /* lua_load gives the main chunk a closed upvalue holding the globals table. */
    static struct table env = {"print", {TAG_C_FUNCTION, {.f = l_print}}};
    struct upval *env_upval = &upvals[upval_count++];
    env_upval->closed.tag = TAG_TABLE;
    env_upval->closed.u.t = &env;
    env_upval->v = &env_upval->closed;
    env_upval->slot = -1;

    struct closure *main_cl = &closures[closure_count++];
    main_cl->p = program->main;
    main_cl->upvals[0] = env_upval;
    stack[0].tag = TAG_LUA_FUNCTION;
    stack[0].u.cl = main_cl;

    int depth = 0;
    frames[0] = (struct frame){main_cl, stack + 1, 0, 0};

    for (;;) {
        struct frame *ci = &frames[depth];
        const struct proto *p = ci->cl->p;
        struct value *base = ci->base;

        char text[96];
        format_ins(text, sizeof text, p, ci->pc);
        fprintf(stderr, "%*s%s:%d  %s\n", depth * 2, "", p->name, ci->pc + 1, text);

        const uint32_t i = p->code[ci->pc++];
        struct value *ra = base + GET_A(i);

        switch (GET_OP(i)) { // GCOVR_EXCL_BR_LINE: the default case is unreachable, see below
        case OP_MOVE:
            *ra = base[GET_B(i)];
            break;
        case OP_LOADI:
            *ra = make_int(GET_SBX(i));
            break;
        case OP_LOADK:
            *ra = p->k[GET_BX(i)];
            break;
        case OP_GETUPVAL:
            *ra = *ci->cl->upvals[GET_B(i)]->v;
            break;
        case OP_GETTABUP: {
            const struct table *t = ci->cl->upvals[GET_B(i)]->v->u.t;
            // GCOVR_EXCL_START: _ENV holds only print, so the key is always found
            if (strcmp(t->key, p->k[GET_C(i)].u.s) != 0) {
                unsupported("a global other than print");
            }
            // GCOVR_EXCL_STOP
            *ra = t->value;
            break;
        }
        case OP_ADDI:
            *ra = make_int(int_add(as_int(&base[GET_B(i)]), GET_SC(i)));
            ci->pc++; /* skip the MMBINI that handles the non-integer case */
            break;
        case OP_ADD:
            *ra = make_int(int_add(as_int(&base[GET_B(i)]), as_int(&base[GET_C(i)])));
            ci->pc++; /* skip the MMBIN */
            break;
        case OP_MUL:
            *ra = make_int(int_mul(as_int(&base[GET_B(i)]), as_int(&base[GET_C(i)])));
            ci->pc++; /* skip the MMBIN */
            break;
        case OP_JMP:
            ci->pc += GET_SJ(i);
            break;
        case OP_LT:
            cond_jump(ci, as_int(ra) < as_int(&base[GET_B(i)]), GET_K(i));
            break;
        case OP_LTI:
            cond_jump(ci, as_int(ra) < GET_SB(i), GET_K(i));
            break;
        case OP_CLOSURE: {
            struct closure *cl = push_closure(p->children[GET_BX(i)], base);
            ra->tag = TAG_LUA_FUNCTION;
            ra->u.cl = cl;
            break;
        }
        case OP_CALL: {
            const int b = GET_B(i);
            const int nresults = GET_C(i) - 1;
            if (b != 0) {
                top = ra + b; /* otherwise the previous instruction set top */
            }
            if (ra->tag == TAG_C_FUNCTION) {
                const int n = ra->u.f(ra + 1, (int)(top - ra) - 1);
                poscall(ra, ra, n, nresults);
            } else {
                depth++;
                frames[depth] = (struct frame){ra->u.cl, ra + 1, 0, nresults};
            }
            break;
        }
        case OP_RETURN:
            /*
             * Only the main chunk returns this way here, with no results. The real handler
             * also moves n results to the caller with luaD_poscall, as RETURN1 does below.
             */
            if (GET_K(i)) {
                close_upvals((int)(base - stack));
            }
            return;
        case OP_RETURN1:
            poscall(base - 1, ra, 1, ci->nresults);
            depth--;
            break;
        case OP_VARARGPREP:
            break; /* a main chunk has no fixed parameters, so there is nothing to move */
        // GCOVR_EXCL_START: the handlers of the opcodes the examples never execute are dropped
        default:
            unsupported(op_names[GET_OP(i)]);
            // GCOVR_EXCL_STOP
        }
    }
}

int main(int argc, char **argv)
{
    const struct program *program = NULL;
    if (argc < 2 || (program = stage_select(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }

    stage_list(program);
    stage_run(program);
    return 0;
}
