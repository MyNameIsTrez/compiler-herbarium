/*
 * wizard: a cut-down version of the in-place interpreter of the Wizard WebAssembly engine.
 *
 * Most interpreters first rewrite the bytecode into a faster private form. Wizard runs the
 * WebAssembly bytes as they are, and gets its speed from one extra array built at validation
 * time: the side table. WebAssembly has structured control flow (block, loop, if), so a branch
 * names a label by its nesting depth, and finding the target in the bytes would mean scanning
 * for the matching `end`. The validator already walks the code with a stack of open blocks and
 * knows, for every branch, where it lands and how many values it must keep and drop. It writes
 * that down as one entry per branch: how far to move the pc, how far to move the side table
 * pointer, how many values to keep and how many to drop below them. At run time every frame
 * carries a pc into the original bytes and a pointer into its function's side table, and a
 * taken branch adds two deltas and slides some values down the stack. The entries are consumed
 * in the order the code reaches them, so a branch that is not taken still moves the pointer
 * past its entry.
 *
 * There is no binary decoder: an example lists the signatures and the raw bytes of each function
 * body, locals declaration and final `end` included, as they sit in a module's code section.
 * Only i32 and i64 values exist, with a small set of instructions. Each oracle program.js holds
 * the same bytes as a whole module and runs it in a real engine.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the results of `main` on stdout, or `trap`, or `invalid` if validation rejects the
 *   module. The intermediate form after each stage goes to stderr. No machine code is made, so
 *   the code.bin argument is ignored.
 *
 * Stages:
 *   stage_input     the signatures and the raw bytes of every function
 *   stage_validate  the validator: types every instruction with an abstract stack, keeps a stack
 *                   of open blocks, and builds the side table of every function
 *   stage_run       the interpreter: runs the bytes in place with a pc and a side table pointer
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIGS 3
#define MAX_FUNCS 2
#define MAX_TABLE 32
#define MAX_CTL 8
#define MAX_STACK 32
#define MAX_LOCALS 8
#define MAX_LABELS 4
#define MAX_FRAMES 16
#define MAX_VALS 1024

enum { T_I32 = 0x7f, T_I64 = 0x7e, T_ANY = 0 };

enum {
    OP_UNREACHABLE = 0x00,
    OP_BLOCK = 0x02,
    OP_LOOP = 0x03,
    OP_IF = 0x04,
    OP_ELSE = 0x05,
    OP_END = 0x0b,
    OP_BR = 0x0c,
    OP_BR_IF = 0x0d,
    OP_BR_TABLE = 0x0e,
    OP_RETURN = 0x0f,
    OP_CALL = 0x10,
    OP_DROP = 0x1a,
    OP_LOCAL_GET = 0x20,
    OP_LOCAL_SET = 0x21,
    OP_LOCAL_TEE = 0x22,
    OP_I32_CONST = 0x41,
    OP_I64_CONST = 0x42,
    OP_I32_EQZ = 0x45,
    OP_I32_LT_S = 0x48,
    OP_I32_ADD = 0x6a,
    OP_I32_SUB = 0x6b,
    OP_I32_MUL = 0x6c,
    OP_I32_DIV_U = 0x6e,
    OP_I64_ADD = 0x7c,
    OP_I32_WRAP_I64 = 0xa7,
};

/* A function type: up to two parameters and two results. A block type that is a type index
 * names one of these too, which is how a block takes parameters. */
struct sig {
    int np, nr;
    uint8_t p[2], r[2];
};

/* One function: its type and the bytes of its body, as they sit in the code section. */
struct func {
    int sig;
    const uint8_t *code;
    int len;
};

struct example {
    const char *name;
    int nsigs;
    struct sig sigs[MAX_SIGS];
    int nfuncs;
    struct func funcs[MAX_FUNCS];
    int entry;
    int nargs;
    int32_t args[2];
};

static const uint8_t arith_f0[] = {
    0x00,                              /* no locals */
    0x20, 0x00,                        /* local.get 0 */
    0x20, 0x01,                        /* local.get 1 */
    0x6a,                              /* i32.add */
    0x41, 0x03,                        /* i32.const 3 */
    0x6c,                              /* i32.mul */
    0x41, 0x02,                        /* i32.const 2 */
    0x6e,                              /* i32.div_u */
    0x0b,                              /* end */
};
static const uint8_t block_br_f0[] = {
    0x00,                              /* no locals */
    0x02, 0x7f,                        /* block i32 */
    0x41, 0xe4, 0x00,                  /* i32.const 100 */
    0x20, 0x00,                        /* local.get 0 */
    0x0d, 0x00,                        /* br_if 0 */
    0x1a,                              /* drop */
    0x41, 0xc8, 0x01,                  /* i32.const 200 */
    0x0b,                              /* end */
    0x0b,                              /* end */
};
static const uint8_t br_drop_f0[] = {
    0x00,                              /* no locals */
    0x02, 0x7f,                        /* block i32 */
    0x41, 0x07,                        /* i32.const 7 */
    0x41, 0x08,                        /* i32.const 8 */
    0x41, 0x09,                        /* i32.const 9 */
    0x20, 0x00,                        /* local.get 0 */
    0x0d, 0x00,                        /* br_if 0 */
    0x1a,                              /* drop */
    0x1a,                              /* drop */
    0x0b,                              /* end */
    0x0b,                              /* end */
};
static const uint8_t loop_sum_f0[] = {
    0x01, 0x01, 0x7f,                  /* locals: 1 x i32 */
    0x02, 0x40,                        /* block */
    0x03, 0x40,                        /* loop */
    0x20, 0x00,                        /* local.get 0 */
    0x45,                              /* i32.eqz */
    0x0d, 0x01,                        /* br_if 1 */
    0x20, 0x01,                        /* local.get 1 */
    0x20, 0x00,                        /* local.get 0 */
    0x6a,                              /* i32.add */
    0x21, 0x01,                        /* local.set 1 */
    0x20, 0x00,                        /* local.get 0 */
    0x41, 0x01,                        /* i32.const 1 */
    0x6b,                              /* i32.sub */
    0x21, 0x00,                        /* local.set 0 */
    0x0c, 0x00,                        /* br 0 */
    0x0b,                              /* end */
    0x0b,                              /* end */
    0x20, 0x01,                        /* local.get 1 */
    0x0b,                              /* end */
};
static const uint8_t loop_params_f0[] = {
    0x01, 0x02, 0x7f,                  /* locals: 2 x i32 */
    0x20, 0x00,                        /* local.get 0 */
    0x03, 0x00,                        /* loop t0 */
    0x20, 0x01,                        /* local.get 1 */
    0x41, 0x01,                        /* i32.const 1 */
    0x6a,                              /* i32.add */
    0x21, 0x01,                        /* local.set 1 */
    0x41, 0x01,                        /* i32.const 1 */
    0x6b,                              /* i32.sub */
    0x22, 0x02,                        /* local.tee 2 */
    0x20, 0x02,                        /* local.get 2 */
    0x0d, 0x00,                        /* br_if 0 */
    0x0b,                              /* end */
    0x1a,                              /* drop */
    0x20, 0x01,                        /* local.get 1 */
    0x0b,                              /* end */
};
static const uint8_t if_else_f0[] = {
    0x00,                              /* no locals */
    0x41, 0xe4, 0x00,                  /* i32.const 100 */
    0x20, 0x00,                        /* local.get 0 */
    0x04, 0x00,                        /* if t0 */
    0x41, 0x01,                        /* i32.const 1 */
    0x6a,                              /* i32.add */
    0x05,                              /* else */
    0x41, 0x02,                        /* i32.const 2 */
    0x6b,                              /* i32.sub */
    0x0b,                              /* end */
    0x0b,                              /* end */
};
static const uint8_t if_else_f1[] = {
    0x00,                              /* no locals */
    0x41, 0x01,                        /* i32.const 1 */
    0x10, 0x00,                        /* call 0 */
    0x41, 0x00,                        /* i32.const 0 */
    0x10, 0x00,                        /* call 0 */
    0x6b,                              /* i32.sub */
    0x0b,                              /* end */
};
static const uint8_t if_no_else_f0[] = {
    0x00,                              /* no locals */
    0x20, 0x00,                        /* local.get 0 */
    0x04, 0x40,                        /* if */
    0x41, 0x05,                        /* i32.const 5 */
    0x21, 0x00,                        /* local.set 0 */
    0x0b,                              /* end */
    0x20, 0x00,                        /* local.get 0 */
    0x0b,                              /* end */
};
static const uint8_t if_no_else_f1[] = {
    0x00,                              /* no locals */
    0x41, 0x00,                        /* i32.const 0 */
    0x10, 0x00,                        /* call 0 */
    0x41, 0x01,                        /* i32.const 1 */
    0x10, 0x00,                        /* call 0 */
    0x6a,                              /* i32.add */
    0x0b,                              /* end */
};
static const uint8_t br_table_f0[] = {
    0x00,                              /* no locals */
    0x02, 0x40,                        /* block */
    0x02, 0x40,                        /* block */
    0x02, 0x40,                        /* block */
    0x20, 0x00,                        /* local.get 0 */
    0x0e, 0x02, 0x00, 0x01, 0x02,      /* br_table 0 1 2 */
    0x0b,                              /* end */
    0x41, 0x0a,                        /* i32.const 10 */
    0x0f,                              /* return */
    0x0b,                              /* end */
    0x41, 0x14,                        /* i32.const 20 */
    0x0f,                              /* return */
    0x0b,                              /* end */
    0x41, 0x1e,                        /* i32.const 30 */
    0x0b,                              /* end */
};
static const uint8_t br_table_f1[] = {
    0x00,                              /* no locals */
    0x41, 0x00,                        /* i32.const 0 */
    0x10, 0x00,                        /* call 0 */
    0x41, 0x01,                        /* i32.const 1 */
    0x10, 0x00,                        /* call 0 */
    0x6a,                              /* i32.add */
    0x41, 0x05,                        /* i32.const 5 */
    0x10, 0x00,                        /* call 0 */
    0x6a,                              /* i32.add */
    0x0b,                              /* end */
};
static const uint8_t multi_result_f0[] = {
    0x00,                              /* no locals */
    0x02, 0x00,                        /* block t0 */
    0x41, 0x09,                        /* i32.const 9 */
    0x41, 0x01,                        /* i32.const 1 */
    0x41, 0x02,                        /* i32.const 2 */
    0x0c, 0x00,                        /* br 0 */
    0x0b,                              /* end */
    0x0b,                              /* end */
};
static const uint8_t fib_f0[] = {
    0x00,                              /* no locals */
    0x20, 0x00,                        /* local.get 0 */
    0x41, 0x02,                        /* i32.const 2 */
    0x48,                              /* i32.lt_s */
    0x04, 0x7f,                        /* if i32 */
    0x20, 0x00,                        /* local.get 0 */
    0x05,                              /* else */
    0x20, 0x00,                        /* local.get 0 */
    0x41, 0x01,                        /* i32.const 1 */
    0x6b,                              /* i32.sub */
    0x10, 0x00,                        /* call 0 */
    0x20, 0x00,                        /* local.get 0 */
    0x41, 0x02,                        /* i32.const 2 */
    0x6b,                              /* i32.sub */
    0x10, 0x00,                        /* call 0 */
    0x6a,                              /* i32.add */
    0x0b,                              /* end */
    0x0b,                              /* end */
};
static const uint8_t fib_f1[] = {
    0x00,                              /* no locals */
    0x41, 0x05,                        /* i32.const 5 */
    0x10, 0x00,                        /* call 0 */
    0x0b,                              /* end */
};
static const uint8_t dead_code_f0[] = {
    0x00,                              /* no locals */
    0x02, 0x7f,                        /* block i32 */
    0x41, 0x01,                        /* i32.const 1 */
    0x0c, 0x00,                        /* br 0 */
    0x0c, 0x00,                        /* br 0 */
    0x6a,                              /* i32.add */
    0x0b,                              /* end */
    0x0b,                              /* end */
};
static const uint8_t return_nested_f0[] = {
    0x00,                              /* no locals */
    0x02, 0x40,                        /* block */
    0x02, 0x40,                        /* block */
    0x41, 0x05,                        /* i32.const 5 */
    0x41, 0x06,                        /* i32.const 6 */
    0x0f,                              /* return */
    0x0b,                              /* end */
    0x0b,                              /* end */
    0x41, 0x00,                        /* i32.const 0 */
    0x0b,                              /* end */
};
static const uint8_t i64_wrap_f0[] = {
    0x00,                              /* no locals */
    0x42, 0x80, 0x80, 0x80, 0x80, 0x10, /* i64.const 4294967296 */
    0x42, 0x07,                        /* i64.const 7 */
    0x7c,                              /* i64.add */
    0xa7,                              /* i32.wrap_i64 */
    0x0b,                              /* end */
};
static const uint8_t trap_div_f0[] = {
    0x00,                              /* no locals */
    0x41, 0x01,                        /* i32.const 1 */
    0x41, 0x00,                        /* i32.const 0 */
    0x6e,                              /* i32.div_u */
    0x0b,                              /* end */
};
static const uint8_t trap_unreachable_f0[] = {
    0x00,                              /* no locals */
    0x00,                              /* unreachable */
    0x0b,                              /* end */
};
static const uint8_t trap_stack_f0[] = {
    0x00,                              /* no locals */
    0x10, 0x00,                        /* call 0 */
    0x0b,                              /* end */
};
static const uint8_t invalid_type_f0[] = {
    0x00,                              /* no locals */
    0x42, 0x01,                        /* i64.const 1 */
    0x41, 0x02,                        /* i32.const 2 */
    0x6a,                              /* i32.add */
    0x0b,                              /* end */
};
static const uint8_t invalid_label_f0[] = {
    0x00,                              /* no locals */
    0x0c, 0x03,                        /* br 3 */
    0x0b,                              /* end */
};
static const uint8_t invalid_opcode_f0[] = {
    0x00,                              /* no locals */
    0xfe,                              /* raw 0xfe */
    0x0b,                              /* end */
};

static const struct example examples[] = {
    {"arith", 1, {{2, 1, {T_I32, T_I32}, {T_I32}}}, 1, {{0, arith_f0, sizeof arith_f0}}, 0, 2, {20, 22}},
    {"block-br", 1, {{1, 1, {T_I32}, {T_I32}}}, 1, {{0, block_br_f0, sizeof block_br_f0}}, 0, 1, {0}},
    {"br-drop", 1, {{1, 1, {T_I32}, {T_I32}}}, 1, {{0, br_drop_f0, sizeof br_drop_f0}}, 0, 1, {1}},
    {"loop-sum", 1, {{1, 1, {T_I32}, {T_I32}}}, 1, {{0, loop_sum_f0, sizeof loop_sum_f0}}, 0, 1, {4}},
    {"loop-params", 1, {{1, 1, {T_I32}, {T_I32}}}, 1, {{0, loop_params_f0, sizeof loop_params_f0}}, 0, 1, {3}},
    {"if-else", 2, {{1, 1, {T_I32}, {T_I32}}, {0, 1, {0}, {T_I32}}}, 2, {{0, if_else_f0, sizeof if_else_f0}, {1, if_else_f1, sizeof if_else_f1}}, 1, 0, {0}},
    {"if-no-else", 2, {{1, 1, {T_I32}, {T_I32}}, {0, 1, {0}, {T_I32}}}, 2, {{0, if_no_else_f0, sizeof if_no_else_f0}, {1, if_no_else_f1, sizeof if_no_else_f1}}, 1, 0, {0}},
    {"br-table", 2, {{1, 1, {T_I32}, {T_I32}}, {0, 1, {0}, {T_I32}}}, 2, {{0, br_table_f0, sizeof br_table_f0}, {1, br_table_f1, sizeof br_table_f1}}, 1, 0, {0}},
    {"multi-result", 1, {{0, 2, {0}, {T_I32, T_I32}}}, 1, {{0, multi_result_f0, sizeof multi_result_f0}}, 0, 0, {0}},
    {"fib", 2, {{1, 1, {T_I32}, {T_I32}}, {0, 1, {0}, {T_I32}}}, 2, {{0, fib_f0, sizeof fib_f0}, {1, fib_f1, sizeof fib_f1}}, 1, 0, {0}},
    {"dead-code", 1, {{0, 1, {0}, {T_I32}}}, 1, {{0, dead_code_f0, sizeof dead_code_f0}}, 0, 0, {0}},
    {"return-nested", 1, {{0, 1, {0}, {T_I32}}}, 1, {{0, return_nested_f0, sizeof return_nested_f0}}, 0, 0, {0}},
    {"i64-wrap", 1, {{0, 1, {0}, {T_I32}}}, 1, {{0, i64_wrap_f0, sizeof i64_wrap_f0}}, 0, 0, {0}},
    {"trap-div", 1, {{0, 1, {0}, {T_I32}}}, 1, {{0, trap_div_f0, sizeof trap_div_f0}}, 0, 0, {0}},
    {"trap-unreachable", 1, {{0, 1, {0}, {T_I32}}}, 1, {{0, trap_unreachable_f0, sizeof trap_unreachable_f0}}, 0, 0, {0}},
    {"trap-stack", 1, {{0, 1, {0}, {T_I32}}}, 1, {{0, trap_stack_f0, sizeof trap_stack_f0}}, 0, 0, {0}},
    {"invalid-type", 1, {{0, 1, {0}, {T_I32}}}, 1, {{0, invalid_type_f0, sizeof invalid_type_f0}}, 0, 0, {0}},
    {"invalid-label", 1, {{0, 0, {0}, {0}}}, 1, {{0, invalid_label_f0, sizeof invalid_label_f0}}, 0, 0, {0}},
    {"invalid-opcode", 1, {{0, 0, {0}, {0}}}, 1, {{0, invalid_opcode_f0, sizeof invalid_opcode_f0}}, 0, 0, {0}},
};

static const struct example *ex;

/* An entry of the side table. `dpc` and `dstp` are deltas from the pc of the branch and from
 * the entry itself. A br_table has a count entry first (its `dpc` is the number of labels
 * besides the default), then one entry per label. */
struct entry {
    int dpc, valcount, popcount, dstp;
};

static struct entry tab[MAX_FUNCS][MAX_TABLE];
static int ntab[MAX_FUNCS];
static int code_start[MAX_FUNCS]; /* pc of the first instruction, after the locals */
static int nlocals[MAX_FUNCS];    /* parameters and declared locals */

/* ---- errors ---- */

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("invalid\n");
        fprintf(stderr, "invalid: %s\n", what);
        exit(0);
    }
}

__attribute__((noreturn)) static void trap(const char *what)
{
    printf("trap\n");
    fprintf(stderr, "trap: %s\n", what);
    exit(0);
}

static void require(bool ok, const char *what)
{
    if (!ok) {
        trap(what);
    }
}

/* ---- decoding ---- */

/* A LEB128 integer of at most nine bytes, signed or not. */
static int64_t read_leb(const uint8_t *code, int len, int *pc, bool is_signed)
{
    uint64_t result = 0;
    int shift = 0;
    uint8_t b;
    do {
        check(*pc < len, "code ends inside an instruction");
        check(shift <= 56, "LEB128 too long");
        b = code[(*pc)++];
        result |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if (is_signed && (b & 0x40)) {
        result |= ~(uint64_t)0 << shift;
    }
    return (int64_t)result;
}

static const char *op_name(uint8_t op)
{
    switch (op) {
    case OP_UNREACHABLE: return "unreachable";
    case OP_BLOCK: return "block";
    case OP_LOOP: return "loop";
    case OP_IF: return "if";
    case OP_ELSE: return "else";
    case OP_END: return "end";
    case OP_BR: return "br";
    case OP_BR_IF: return "br_if";
    case OP_BR_TABLE: return "br_table";
    case OP_RETURN: return "return";
    case OP_CALL: return "call";
    case OP_DROP: return "drop";
    case OP_LOCAL_GET: return "local.get";
    case OP_LOCAL_SET: return "local.set";
    case OP_LOCAL_TEE: return "local.tee";
    case OP_I32_CONST: return "i32.const";
    case OP_I64_CONST: return "i64.const";
    case OP_I32_EQZ: return "i32.eqz";
    case OP_I32_LT_S: return "i32.lt_s";
    case OP_I32_ADD: return "i32.add";
    case OP_I32_SUB: return "i32.sub";
    case OP_I32_MUL: return "i32.mul";
    case OP_I32_DIV_U: return "i32.div_u";
    case OP_I64_ADD: return "i64.add";
    case OP_I32_WRAP_I64: return "i32.wrap_i64";
    default: return "unknown";
    }
}

static const char *type_name(uint8_t t)
{
    return t == T_I32 ? "i32" : "i64";
}

static void print_types(const uint8_t *t, int n)
{
    fprintf(stderr, "(");
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "%s%s", i ? " " : "", type_name(t[i]));
    }
    fprintf(stderr, ")");
}

/* ---- stage_input ---- */

static void stage_input(void)
{
    fprintf(stderr, "== stage_input ==\n");
    for (int i = 0; i < ex->nsigs; i++) {
        fprintf(stderr, "sig %d: ", i);
        print_types(ex->sigs[i].p, ex->sigs[i].np);
        fprintf(stderr, " -> ");
        print_types(ex->sigs[i].r, ex->sigs[i].nr);
        fprintf(stderr, "\n");
    }
    for (int i = 0; i < ex->nfuncs; i++) {
        const struct func *f = &ex->funcs[i];
        fprintf(stderr, "func %d: sig %d, %d bytes:", i, f->sig, f->len);
        for (int k = 0; k < f->len; k++) {
            fprintf(stderr, " %02x", f->code[k]);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "main: func %d, %d arguments\n\n", ex->entry, ex->nargs);
}

/* ---- stage_validate ---- */

/* An open block. `base` is the height of the value stack under the block's parameters.
 * `first_ref` heads a list of the entries that branch to this block's end (or, for a loop, to
 * its start) and are not yet resolved; `else_ref` is the entry of an `if` that jumps to the else
 * arm. While an entry waits, its `dpc` holds the pc of its branch and its `dstp` the next entry
 * of the list. */
struct ctl {
    uint8_t opcode;
    int np, nr;
    uint8_t params[2], results[2];
    int base;
    bool reachable;
    int first_ref;
    int else_ref;
    int start_pos;
    int stp_start;
};

struct validator {
    int fn;
    const struct func *func;
    const uint8_t *code;
    int len;
    int pc;
    uint8_t locals[MAX_LOCALS];
    int nlocals;
    uint8_t stack[MAX_STACK];
    int top;
    struct ctl ctl[MAX_CTL];
    int nctl;
    struct entry *tab;
    int *ntab;
};

static void push(struct validator *v, uint8_t t)
{
    check(v->top < MAX_STACK, "value stack too deep");
    v->stack[v->top++] = t;
}

/* Pops a value that must have type `want`, or any type for T_ANY. At the base of an
 * unreachable block the stack is polymorphic: it yields values of whatever type is wanted. */
static uint8_t pop(struct validator *v, uint8_t want)
{
    const struct ctl *c = &v->ctl[v->nctl - 1];
    uint8_t got = T_ANY;
    if (v->top == c->base) {
        check(!c->reachable, "value stack underflow");
    } else {
        got = v->stack[--v->top];
    }
    check(want == T_ANY || got == T_ANY || got == want, "type mismatch");
    return got == T_ANY ? want : got;
}

static struct ctl *get_ctl(struct validator *v, int depth)
{
    check(depth < v->nctl, "unknown label");
    return &v->ctl[v->nctl - 1 - depth];
}

/* A branch to a loop passes the loop's parameters; a branch to anything else its results. */
static int label_arity(const struct ctl *c)
{
    return c->opcode == OP_LOOP ? c->np : c->nr;
}

static const uint8_t *label_types(const struct ctl *c)
{
    return c->opcode == OP_LOOP ? c->params : c->results;
}

static void set_unreachable(struct validator *v)
{
    struct ctl *c = &v->ctl[v->nctl - 1];
    c->reachable = false;
    v->top = c->base;
}

/* Adds an entry to the side table. */
static int put_entry(struct validator *v, int dpc, int valcount, int popcount, int dstp)
{
    check(*v->ntab < MAX_TABLE, "side table full");
    int e = (*v->ntab)++;
    v->tab[e] = (struct entry){dpc, valcount, popcount, dstp};
    return e;
}

/* The entry of a branch to `target` made at `pos`. The target is not known yet, so the entry
 * joins the list of unresolved references of the target. `valcount` values are kept and the
 * ones above the target's base under them are dropped. */
static void ref_branch(struct validator *v, struct ctl *target, int pos)
{
    int valcount = label_arity(target);
    int popcount = v->top - valcount - target->base;
    if (popcount < 0) {
        popcount = 0; /* dead code with a short stack: the entry is never used */
    }
    target->first_ref = put_entry(v, pos, valcount, popcount, target->first_ref);
}

/* A branch that moves no values. */
static void ref_goto(struct validator *v, struct ctl *target, int pos)
{
    target->first_ref = put_entry(v, pos, 0, 0, target->first_ref);
}

/* Fills in an entry now that its target is known. */
static void bind_entry(struct validator *v, int e, int target_pc, int target_stp)
{
    struct entry *en = &v->tab[e];
    en->dpc = target_pc - en->dpc;
    en->dstp = target_stp - e;
}

/* Resolves every branch that waits on `c`. */
static void bind(struct validator *v, struct ctl *c, int target_pc, int target_stp)
{
    int e = c->first_ref;
    while (e >= 0) {
        int next = v->tab[e].dstp;
        bind_entry(v, e, target_pc, target_stp);
        e = next;
    }
    c->first_ref = -1;
}

/* The values a branch passes must match the label's types; they stay on the stack. */
static void check_label_args(struct validator *v, const struct ctl *target)
{
    int n = label_arity(target);
    const uint8_t *types = label_types(target);
    for (int i = n - 1; i >= 0; i--) {
        pop(v, types[i]);
    }
    for (int i = 0; i < n; i++) {
        push(v, types[i]);
    }
}

/* Opens a block: reads its block type, takes its parameters off the stack and puts them back
 * on top of a new base. */
static struct ctl *begin_block(struct validator *v, uint8_t op, int pos)
{
    check(v->nctl < MAX_CTL, "blocks nested too deep");
    int64_t bt = read_leb(v->code, v->len, &v->pc, true);
    struct ctl *c = &v->ctl[v->nctl];
    *c = (struct ctl){.opcode = op, .reachable = true, .first_ref = -1, .else_ref = -1, .start_pos = pos};
    if (bt == -64) {
        /* 0x40: no parameters, no results */
    } else if (bt < 0) {
        c->nr = 1;
        c->results[0] = (uint8_t)(bt & 0x7f);
    } else {
        check(bt < ex->nsigs, "unknown block type");
        const struct sig *s = &ex->sigs[bt];
        c->np = s->np;
        c->nr = s->nr;
        memcpy(c->params, s->p, sizeof c->params);
        memcpy(c->results, s->r, sizeof c->results);
    }
    for (int i = c->np - 1; i >= 0; i--) {
        pop(v, c->params[i]);
    }
    c->base = v->top;
    c->stp_start = *v->ntab;
    v->nctl++;
    for (int i = 0; i < c->np; i++) {
        push(v, c->params[i]);
    }
    return c;
}

/* The values at the end of an arm must be exactly the block's results. */
static void check_results(struct validator *v, const struct ctl *c)
{
    for (int i = c->nr - 1; i >= 0; i--) {
        pop(v, c->results[i]);
    }
    check(v->top == c->base, "values left on the stack at the end of a block");
}

static void unop(struct validator *v, uint8_t in, uint8_t out)
{
    pop(v, in);
    push(v, out);
}

static void binop(struct validator *v, uint8_t in, uint8_t out)
{
    pop(v, in);
    pop(v, in);
    push(v, out);
}

static void print_stack(const struct validator *v)
{
    fprintf(stderr, "[");
    for (int i = 0; i < v->top; i++) {
        fprintf(stderr, "%s%s", i ? " " : "", type_name(v->stack[i]));
    }
    fprintf(stderr, "]");
}

static void validate_func(int fi)
{
    const struct func *f = &ex->funcs[fi];
    const struct sig *sig = &ex->sigs[f->sig];
    struct validator v = {.fn = fi, .func = f, .code = f->code, .len = f->len, .tab = tab[fi], .ntab = &ntab[fi]};

    /* the locals: the parameters, then the declared ones */
    for (int i = 0; i < sig->np; i++) {
        v.locals[v.nlocals++] = sig->p[i];
    }
    int ndecls = (int)read_leb(v.code, v.len, &v.pc, false);
    for (int i = 0; i < ndecls; i++) {
        int count = (int)read_leb(v.code, v.len, &v.pc, false);
        check(v.pc < v.len, "code ends inside the locals");
        check(v.code[v.pc++] == T_I32, "only i32 locals are supported");
        check(v.nlocals + count <= MAX_LOCALS, "too many locals");
        for (int k = 0; k < count; k++) {
            v.locals[v.nlocals++] = T_I32;
        }
    }
    code_start[fi] = v.pc;
    nlocals[fi] = v.nlocals;

    /* the function body is a block whose results are the function's results */
    v.ctl[0] = (struct ctl){.opcode = 0, .nr = sig->nr, .reachable = true, .first_ref = -1, .else_ref = -1};
    memcpy(v.ctl[0].results, sig->r, sizeof sig->r);
    v.nctl = 1;

    fprintf(stderr, "func %d:\n", fi);
    while (v.nctl > 0) {
        check(v.pc < v.len, "code ends before the function's end");
        int pos = v.pc;
        uint8_t op = v.code[v.pc++];
        int entries_before = *v.ntab;
        char imm[64] = "";
        switch (op) {
        case OP_UNREACHABLE:
            set_unreachable(&v);
            break;
        case OP_BLOCK:
        case OP_LOOP: {
            struct ctl *c = begin_block(&v, op, pos);
            snprintf(imm, sizeof imm, "params %d results %d", c->np, c->nr);
            break;
        }
        case OP_IF: {
            pop(&v, T_I32);
            struct ctl *c = begin_block(&v, op, pos);
            /* the entry taken when the condition is false, bound at the else or the end */
            c->else_ref = put_entry(&v, pos, 0, 0, -1);
            snprintf(imm, sizeof imm, "params %d results %d", c->np, c->nr);
            break;
        }
        case OP_ELSE: {
            struct ctl *c = &v.ctl[v.nctl - 1];
            check(c->else_ref >= 0, "else without if");
            check_results(&v, c);
            /* the then arm ends by jumping to the end; the false branch lands after this opcode */
            ref_goto(&v, c, pos);
            bind_entry(&v, c->else_ref, pos + 1, *v.ntab);
            c->else_ref = -1;
            v.top = c->base;
            for (int i = 0; i < c->np; i++) {
                push(&v, c->params[i]);
            }
            c->reachable = true;
            break;
        }
        case OP_END: {
            struct ctl *c = &v.ctl[v.nctl - 1];
            check_results(&v, c);
            v.top = c->base;
            for (int i = 0; i < c->nr; i++) {
                push(&v, c->results[i]);
            }
            if (c->else_ref >= 0) {
                /* an if without an else behaves as if it had an empty else arm */
                check(c->np == c->nr, "if without else must keep its types");
                check(memcmp(c->params, c->results, (size_t)c->nr) == 0, "if without else must keep its types");
                bind_entry(&v, c->else_ref, pos, *v.ntab);
            }
            if (c->opcode == OP_LOOP) {
                bind(&v, c, c->start_pos, c->stp_start);
            } else {
                bind(&v, c, pos, *v.ntab);
            }
            v.nctl--;
            break;
        }
        case OP_BR: {
            int depth = (int)read_leb(v.code, v.len, &v.pc, false);
            struct ctl *t = get_ctl(&v, depth);
            ref_branch(&v, t, pos);
            check_label_args(&v, t);
            set_unreachable(&v);
            snprintf(imm, sizeof imm, "%d", depth);
            break;
        }
        case OP_BR_IF: {
            int depth = (int)read_leb(v.code, v.len, &v.pc, false);
            struct ctl *t = get_ctl(&v, depth);
            pop(&v, T_I32);
            ref_branch(&v, t, pos);
            check_label_args(&v, t);
            snprintf(imm, sizeof imm, "%d", depth);
            break;
        }
        case OP_BR_TABLE: {
            pop(&v, T_I32);
            int n = (int)read_leb(v.code, v.len, &v.pc, false);
            check(n < MAX_LABELS, "too many br_table labels");
            int depths[MAX_LABELS];
            for (int i = 0; i <= n; i++) {
                depths[i] = (int)read_leb(v.code, v.len, &v.pc, false);
            }
            put_entry(&v, n, 0, 0, 0);
            int arity = label_arity(get_ctl(&v, depths[n]));
            for (int i = 0; i <= n; i++) {
                struct ctl *t = get_ctl(&v, depths[i]);
                check(label_arity(t) == arity, "br_table labels have different arities");
                /* the entry of label i is at table slot i + 1, so its branch pc is pos + 1 + i */
                ref_branch(&v, t, pos + 1 + i);
                check_label_args(&v, t);
            }
            set_unreachable(&v);
            snprintf(imm, sizeof imm, "%d labels and a default", n);
            break;
        }
        case OP_RETURN:
            for (int i = sig->nr - 1; i >= 0; i--) {
                pop(&v, sig->r[i]);
            }
            set_unreachable(&v);
            break;
        case OP_CALL: {
            int idx = (int)read_leb(v.code, v.len, &v.pc, false);
            check(idx < ex->nfuncs, "unknown function");
            const struct sig *cs = &ex->sigs[ex->funcs[idx].sig];
            for (int i = cs->np - 1; i >= 0; i--) {
                pop(&v, cs->p[i]);
            }
            for (int i = 0; i < cs->nr; i++) {
                push(&v, cs->r[i]);
            }
            snprintf(imm, sizeof imm, "%d", idx);
            break;
        }
        case OP_DROP:
            pop(&v, T_ANY);
            break;
        case OP_LOCAL_GET:
        case OP_LOCAL_SET:
        case OP_LOCAL_TEE: {
            int idx = (int)read_leb(v.code, v.len, &v.pc, false);
            check(idx < v.nlocals, "unknown local");
            if (op == OP_LOCAL_GET) {
                push(&v, v.locals[idx]);
            } else {
                pop(&v, v.locals[idx]);
                if (op == OP_LOCAL_TEE) {
                    push(&v, v.locals[idx]);
                }
            }
            snprintf(imm, sizeof imm, "%d", idx);
            break;
        }
        case OP_I32_CONST:
        case OP_I64_CONST: {
            int64_t k = read_leb(v.code, v.len, &v.pc, true);
            push(&v, op == OP_I32_CONST ? T_I32 : T_I64);
            snprintf(imm, sizeof imm, "%lld", (long long)k);
            break;
        }
        case OP_I32_EQZ:
            unop(&v, T_I32, T_I32);
            break;
        case OP_I32_LT_S:
        case OP_I32_ADD:
        case OP_I32_SUB:
        case OP_I32_MUL:
        case OP_I32_DIV_U:
            binop(&v, T_I32, T_I32);
            break;
        case OP_I64_ADD:
            binop(&v, T_I64, T_I64);
            break;
        case OP_I32_WRAP_I64:
            unop(&v, T_I64, T_I32);
            break;
        default:
            fprintf(stderr, "  %3d  %s\n", pos, op_name(op));
            check(false, "unknown opcode");
        }
        fprintf(stderr, "  %3d  %-13s %-22s ", pos, op_name(op), imm);
        print_stack(&v);
        if (*v.ntab != entries_before) {
            fprintf(stderr, "  entries %d..%d", entries_before, *v.ntab - 1);
        }
        fprintf(stderr, "\n");
    }
    check(v.pc == v.len, "bytes after the function's end");

    fprintf(stderr, "  side table:\n");
    for (int e = 0; e < ntab[fi]; e++) {
        const struct entry *en = &tab[fi][e];
        fprintf(stderr, "    %2d: dpc %+d, dstp %+d, valcount %d, popcount %d\n", e, en->dpc, en->dstp, en->valcount,
                en->popcount);
    }
}

static void stage_validate(void)
{
    fprintf(stderr, "== stage_validate ==\n");
    for (int i = 0; i < ex->nfuncs; i++) {
        validate_func(i);
    }
    fprintf(stderr, "\n");
}

/* ---- stage_run ---- */

struct frame {
    int fn, pc, stp, fp;
};

static int64_t vals[MAX_VALS];
static int top;
static struct frame frames[MAX_FRAMES];
static int nframes;

static void push_frame(int fn)
{
    const struct sig *s = &ex->sigs[ex->funcs[fn].sig];
    require(nframes < MAX_FRAMES, "call stack exhausted");
    struct frame *f = &frames[nframes++];
    *f = (struct frame){.fn = fn, .pc = code_start[fn], .stp = 0, .fp = top - s->np};
    for (int i = s->np; i < nlocals[fn]; i++) {
        vals[top++] = 0;
    }
    fprintf(stderr, "  call func %d, depth %d\n", fn, nframes);
}

/* Takes the branch of the entry at the side table pointer: keep the top `valcount` values,
 * drop `popcount` values under them, then move the pc and the side table pointer. */
static void take_branch(struct frame *f, const char *what)
{
    const struct entry *e = &tab[f->fn][f->stp];
    fprintf(stderr, "  %-8s pc %d -> %d, stp %d -> %d, keep %d, drop %d\n", what, f->pc, f->pc + e->dpc, f->stp,
            f->stp + e->dstp, e->valcount, e->popcount);
    memmove(&vals[top - e->popcount - e->valcount], &vals[top - e->valcount], (size_t)e->valcount * sizeof vals[0]);
    top -= e->popcount;
    f->pc += e->dpc;
    f->stp += e->dstp;
}

/* A branch that is not taken still steps over its entry. */
static void skip_entry(struct frame *f, const char *what)
{
    fprintf(stderr, "  %-8s not taken, stp %d -> %d\n", what, f->stp, f->stp + 1);
    f->stp++;
}

static void do_return(struct frame *f)
{
    const struct sig *s = &ex->sigs[ex->funcs[f->fn].sig];
    memmove(&vals[f->fp], &vals[top - s->nr], (size_t)s->nr * sizeof vals[0]);
    top = f->fp + s->nr;
    fprintf(stderr, "  return func %d, depth %d\n", f->fn, nframes);
    nframes--;
}

static int32_t i32(int64_t x)
{
    return (int32_t)x;
}

static void stage_run(void)
{
    fprintf(stderr, "== stage_run ==\n");
    for (int i = 0; i < ex->nargs; i++) {
        vals[top++] = ex->args[i];
    }
    push_frame(ex->entry);
    while (nframes > 0) {
        struct frame *f = &frames[nframes - 1];
        const struct func *fn = &ex->funcs[f->fn];
        const uint8_t *code = fn->code;
        uint8_t op = code[f->pc];
        int next = f->pc + 1;
        switch (op) {
        case OP_UNREACHABLE:
            trap("unreachable executed");
        case OP_BLOCK:
        case OP_LOOP:
            read_leb(code, fn->len, &next, true);
            f->pc = next;
            break;
        case OP_IF:
            read_leb(code, fn->len, &next, true);
            if (i32(vals[--top]) != 0) {
                skip_entry(f, "if");
                f->pc = next;
            } else {
                take_branch(f, "if");
            }
            break;
        case OP_ELSE:
            take_branch(f, "else");
            break;
        case OP_END:
            if (next == fn->len) {
                do_return(f);
            } else {
                f->pc = next;
            }
            break;
        case OP_BR:
            take_branch(f, "br");
            break;
        case OP_BR_IF:
            if (i32(vals[--top]) != 0) {
                take_branch(f, "br_if");
            } else {
                read_leb(code, fn->len, &next, false);
                skip_entry(f, "br_if");
                f->pc = next;
            }
            break;
        case OP_BR_TABLE: {
            uint32_t key = (uint32_t)vals[--top];
            uint32_t n = (uint32_t)tab[f->fn][f->stp].dpc;
            /* the entry of the label is `index` slots after the count entry, and its branch pc is
             * `index` bytes after the opcode */
            uint32_t index = key < n ? key + 1 : n + 1;
            f->stp += (int)index;
            f->pc += (int)index;
            take_branch(f, "br_table");
            break;
        }
        case OP_RETURN:
            do_return(f);
            break;
        case OP_CALL: {
            int idx = (int)read_leb(code, fn->len, &next, false);
            f->pc = next; /* where the caller continues */
            push_frame(idx);
            break;
        }
        case OP_DROP:
            top--;
            f->pc = next;
            break;
        case OP_LOCAL_GET:
            vals[top++] = vals[f->fp + read_leb(code, fn->len, &next, false)];
            f->pc = next;
            break;
        case OP_LOCAL_SET:
            vals[f->fp + read_leb(code, fn->len, &next, false)] = vals[--top];
            f->pc = next;
            break;
        case OP_LOCAL_TEE:
            vals[f->fp + read_leb(code, fn->len, &next, false)] = vals[top - 1];
            f->pc = next;
            break;
        case OP_I32_CONST:
            vals[top++] = i32(read_leb(code, fn->len, &next, true));
            f->pc = next;
            break;
        case OP_I64_CONST:
            vals[top++] = read_leb(code, fn->len, &next, true);
            f->pc = next;
            break;
        case OP_I32_EQZ:
            vals[top - 1] = i32(vals[top - 1]) == 0;
            f->pc = next;
            break;
        case OP_I32_LT_S:
            top--;
            vals[top - 1] = i32(vals[top - 1]) < i32(vals[top]);
            f->pc = next;
            break;
        case OP_I32_ADD:
            top--;
            vals[top - 1] = i32((int64_t)((uint32_t)vals[top - 1] + (uint32_t)vals[top]));
            f->pc = next;
            break;
        case OP_I32_SUB:
            top--;
            vals[top - 1] = i32((int64_t)((uint32_t)vals[top - 1] - (uint32_t)vals[top]));
            f->pc = next;
            break;
        case OP_I32_MUL:
            top--;
            vals[top - 1] = i32((int64_t)((uint32_t)vals[top - 1] * (uint32_t)vals[top]));
            f->pc = next;
            break;
        case OP_I32_DIV_U:
            top--;
            require((uint32_t)vals[top] != 0, "integer divide by zero");
            vals[top - 1] = i32((int64_t)((uint32_t)vals[top - 1] / (uint32_t)vals[top]));
            f->pc = next;
            break;
        case OP_I64_ADD:
            top--;
            vals[top - 1] = (int64_t)((uint64_t)vals[top - 1] + (uint64_t)vals[top]);
            f->pc = next;
            break;
        default: /* OP_I32_WRAP_I64: the validator rejected every other opcode */
            vals[top - 1] = i32(vals[top - 1]);
            f->pc = next;
            break;
        }
    }
    for (int i = 0; i < top; i++) {
        printf("%s%lld", i ? " " : "", (long long)vals[i]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    ex = NULL;
    for (size_t i = 0; argc >= 2 && i < sizeof examples / sizeof examples[0]; i++) {
        if (strcmp(argv[1], examples[i].name) == 0) {
            ex = &examples[i];
        }
    }
    if (ex == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_input();
    stage_validate();
    stage_run();
    return 0;
}
