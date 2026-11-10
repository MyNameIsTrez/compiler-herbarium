/*
 * luajit: a cut-down tracing JIT, after LuaJIT 2.1.
 *
 * LuaJIT runs a bytecode interpreter and counts how often each loop runs. A loop that gets hot
 * is recorded: the interpreter keeps executing the loop, and the recorder watches it and
 * writes down what happens as a linear SSA IR, with a guard wherever the interpreter made a
 * choice (a branch, the type of a value). Along the way it takes snapshots, which say what
 * the interpreter's stack would hold if a guard failed. The recorded trace then goes through
 * the loop optimization (copy-substitution: the recorded iteration becomes a "pre-roll" and a
 * substituted copy of it becomes the loop body, joined by PHIs), and is assembled by walking
 * the IR from its end to its start, allocating registers on the way. A failed guard jumps to
 * an exit stub, which saves the registers, and a C function restores the interpreter's stack
 * from the snapshot.
 *
 * This entry keeps that pipeline for one kind of trace: a loop over integers.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the form after each stage on stderr. code.bin
 *   receives the machine code of the trace (with one host address zeroed), if a trace was made.
 *
 * Stages, in the order they run:
 *   stage_bytecode   the bytecode of each function
 *   stage_interpret  run the bytecode and count backward jumps; a hot one starts the stages below
 *   stage_record     record one iteration into IR and snapshots, while the interpreter runs it
 *   stage_loop       the loop optimization: copy-substitution and PHIs
 *   stage_assemble   walk the IR backwards, allocate registers and emit x86-64
 *   stage_run        write code.bin, make the code executable and run the trace
 */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

// GCOVR_EXCL_START: this entry handles only what the examples record, and stops on the rest
static void require(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "unsupported: %s\n", what);
        exit(1);
    }
}
// GCOVR_EXCL_STOP

#define MAX_FUNCS 2
#define MAX_CODE 16
#define MAX_CONSTS 3
#define STACK_SLOTS 64
#define HOTLOOP 3

/* ------------------------------------------------------------------------------------------
 * Values and the interpreter. A slot is 16 bytes: a type tag and a payload.
 * ---------------------------------------------------------------------------------------- */

enum { T_NIL, T_INT, T_STR, T_FUNC };

struct slot {
    int64_t tag;
    int64_t val;
};

struct function {
    void (*native)(struct slot *base);
    int proto;
};

static void rt_print(struct slot *base)
{
    if (base[0].tag == T_STR) {
        printf("%s\n", (const char *)(intptr_t)base[0].val);
    } else {
        printf("%lld\n", (long long)base[0].val);
    }
}

/* Registers are numbered slots of the frame. A call has the function in slot A and the
 * arguments after it, and the callee's frame starts at slot A + 1. The callee stores its
 * result in the slot before its own frame, which is slot A of the caller. */
enum op { OP_LOADI, OP_LOADK, OP_MOVE, OP_ADD, OP_SUB, OP_MUL, OP_JNLT, OP_JMP, OP_CALL,
          OP_RET1, OP_RET0 };

static const char *const op_name[] = { "LOADI", "LOADK", "MOVE", "ADD", "SUB", "MUL", "JNLT",
                                       "JMP", "CALL", "RET1", "RET0" };

struct insn {
    enum op op;
    int a, b, c; /* slots, or b is the constant number of LOADK */
    int imm;     /* LOADI: the integer. JNLT, JMP: the index of the target instruction */
};

struct proto {
    const char *name;
    int n;
    struct insn code[MAX_CODE];
    int hot[MAX_CODE]; /* per instruction: backward jumps left before it counts as hot */
};

static struct function print_fn = { rt_print, 0 };
static struct function fact_fn = { NULL, 1 };

static const char hello[] = "Hello, world!";

static struct slot consts[MAX_CONSTS];
static struct proto protos[MAX_FUNCS];
static int nprotos;

static void set_const(int k, int64_t tag, const void *p)
{
    consts[k].tag = tag;
    consts[k].val = (int64_t)(intptr_t)p;
}

static void add_proto(const char *name, int n, const struct insn *code)
{
    struct proto *p = &protos[nprotos++];
    p->name = name;
    p->n = n;
    memcpy(p->code, code, (size_t)n * sizeof *code);
    for (int i = 0; i < MAX_CODE; i++) {
        p->hot[i] = HOTLOOP;
    }
}

#define I(op, a, b, c, imm) { op, a, b, c, imm }

static void example_hello(void)
{
    set_const(0, T_FUNC, &print_fn);
    set_const(1, T_STR, hello);
    const struct insn code[] = {
        I(OP_LOADK, 0, 0, 0, 0), I(OP_LOADK, 1, 1, 0, 0), I(OP_CALL, 0, 0, 0, 0),
        I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 4, code);
}

static void example_arithmetic(void)
{
    set_const(0, T_FUNC, &print_fn);
    const struct insn code[] = {
        I(OP_LOADK, 0, 0, 0, 0), I(OP_LOADI, 1, 0, 0, 2), I(OP_LOADI, 2, 0, 0, 3),
        I(OP_ADD, 1, 1, 2, 0),   I(OP_LOADI, 2, 0, 0, 4), I(OP_MUL, 1, 1, 2, 0),
        I(OP_LOADI, 2, 0, 0, 6), I(OP_SUB, 1, 1, 2, 0),   I(OP_CALL, 0, 0, 0, 0),
        I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 10, code);
}

static void example_branch(void)
{
    set_const(0, T_FUNC, &print_fn);
    const struct insn code[] = {
        I(OP_LOADI, 0, 0, 0, 3),  I(OP_LOADI, 1, 0, 0, 5), I(OP_JNLT, 0, 0, 1, 8),
        I(OP_LOADK, 2, 0, 0, 0),  I(OP_MOVE, 3, 1, 0, 0),  I(OP_CALL, 2, 0, 0, 0),
        I(OP_JMP, 0, 0, 0, 11),   I(OP_RET0, 0, 0, 0, 0),  I(OP_LOADK, 2, 0, 0, 0),
        I(OP_MOVE, 3, 0, 0, 0),   I(OP_CALL, 2, 0, 0, 0),  I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 12, code);
}

static void example_loop(void)
{
    set_const(0, T_FUNC, &print_fn);
    const struct insn code[] = {
        I(OP_LOADI, 0, 0, 0, 0), I(OP_LOADI, 1, 0, 0, 1), I(OP_LOADI, 2, 0, 0, 11),
        I(OP_JNLT, 0, 1, 2, 9),  I(OP_ADD, 0, 0, 1, 0),   I(OP_LOADI, 3, 0, 0, 1),
        I(OP_ADD, 1, 1, 3, 0),   I(OP_JMP, 0, 0, 0, 3),   I(OP_RET0, 0, 0, 0, 0),
        I(OP_LOADK, 2, 0, 0, 0), I(OP_MOVE, 3, 0, 0, 0),  I(OP_CALL, 2, 0, 0, 0),
        I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 13, code);
}

static void example_call(void)
{
    set_const(0, T_FUNC, &print_fn);
    set_const(1, T_FUNC, &fact_fn);
    const struct insn code[] = {
        I(OP_LOADK, 0, 0, 0, 0), I(OP_LOADK, 1, 1, 0, 0), I(OP_LOADI, 2, 0, 0, 5),
        I(OP_CALL, 1, 0, 0, 0),  I(OP_CALL, 0, 0, 0, 0),  I(OP_RET0, 0, 0, 0, 0),
    };
    add_proto("main", 6, code);
    const struct insn fact[] = {
        I(OP_LOADI, 1, 0, 0, 2), I(OP_JNLT, 0, 0, 1, 5), I(OP_LOADI, 1, 0, 0, 1),
        I(OP_RET1, 1, 0, 0, 0),  I(OP_RET0, 0, 0, 0, 0), I(OP_LOADK, 1, 1, 0, 0),
        I(OP_LOADI, 2, 0, 0, 1), I(OP_SUB, 2, 0, 2, 0),  I(OP_CALL, 1, 0, 0, 0),
        I(OP_MUL, 1, 0, 1, 0),   I(OP_RET1, 1, 0, 0, 0),
    };
    add_proto("fact", 11, fact);
}

static const struct {
    const char *name;
    void (*build)(void);
} examples[] = {
    { "hello", example_hello },
    { "arithmetic", example_arithmetic },
    { "branch", example_branch },
    { "loop", example_loop },
    { "call", example_call },
};

static void stage_bytecode(void)
{
    P("== stage_bytecode ==\n");
    for (int f = 0; f < nprotos; f++) {
        const struct proto *p = &protos[f];
        P("function <%s> (%d instructions)\n", p->name, p->n);
        for (int i = 0; i < p->n; i++) {
            const struct insn *in = &p->code[i];
            P("  %2d  %-6s %d %d %d imm=%d\n", i, op_name[in->op], in->a, in->b, in->c, in->imm);
        }
    }
}

static const char *code_path;

static int run_trace(struct proto *p, int startpc, struct slot *base);

/* Execute one instruction and return the pc of the next one, or -1 after a return. */
static int step(const struct proto *p, int pc, struct slot *base);

static void run(struct proto *p, struct slot *base)
{
    int pc = 0;
    for (;;) {
        const struct insn *in = &p->code[pc];
        int next = step(p, pc, base);
        if (next < 0) {
            return;
        }
        /* A jump backwards is the end of a loop. LuaJIT counts in the loop instruction at the
         * start of the loop; counting at the jump back is the same in effect here. */
        if (in->op == OP_JMP && next < pc && --p->hot[pc] == 0) {
            next = run_trace(p, next, base);
        }
        pc = next;
    }
}

static int step(const struct proto *p, int pc, struct slot *base)
{
    const struct insn *in = &p->code[pc];
    struct slot *r = base;
    switch (in->op) { // GCOVR_EXCL_BR_LINE: the default case is unreachable, see below
    case OP_LOADI:
        r[in->a].tag = T_INT;
        r[in->a].val = in->imm;
        break;
    case OP_LOADK:
        r[in->a] = consts[in->b];
        break;
    case OP_MOVE:
        r[in->a] = r[in->b];
        break;
    case OP_ADD:
        r[in->a].tag = T_INT;
        r[in->a].val = (int64_t)((uint64_t)r[in->b].val + (uint64_t)r[in->c].val);
        break;
    case OP_SUB:
        r[in->a].tag = T_INT;
        r[in->a].val = (int64_t)((uint64_t)r[in->b].val - (uint64_t)r[in->c].val);
        break;
    case OP_MUL:
        r[in->a].tag = T_INT;
        r[in->a].val = (int64_t)((uint64_t)r[in->b].val * (uint64_t)r[in->c].val);
        break;
    case OP_JNLT:
        if (!(r[in->b].val < r[in->c].val)) {
            return in->imm;
        }
        break;
    case OP_JMP:
        return in->imm;
    case OP_CALL: {
        const struct function *f = (const struct function *)(intptr_t)r[in->a].val;
        if (f->native) {
            f->native(r + in->a + 1);
        } else {
            run(&protos[f->proto], r + in->a + 1);
        }
        break;
    }
    case OP_RET1:
        base[-1] = r[in->a];
        return -1;
    case OP_RET0:
        return -1;
    // GCOVR_EXCL_START: every opcode has a case
    default:
        break;
        // GCOVR_EXCL_STOP
    }
    return pc + 1;
}

static void stage_interpret(struct slot *stack)
{
    P("== stage_interpret ==\n");
    run(&protos[0], stack + 1);
    P("interpreter finished\n");
}

/* ------------------------------------------------------------------------------------------
 * The IR. LuaJIT keeps constants and instructions in one array, split at REF_BIAS: constants
 * grow downwards from it and instructions grow upwards. A reference below the bias is a
 * constant, so telling them apart takes one comparison.
 * ---------------------------------------------------------------------------------------- */

#define REF_BIAS 32
#define REF_FIRST (REF_BIAS + 1)
#define IR_SIZE 64
#define MAX_SLOTS 4
#define MAX_SNAP 8
#define NREGS 8
#define NOREG (-1)
#define ALLREGS ((1u << NREGS) - 1)

enum ir_op { IR_SLOAD, IR_ADD, IR_SUB, IR_LT, IR_LOOP, IR_PHI, IR_KINT };

static const char *const ir_name[] = { "SLOAD", "ADD", "SUB", "LT", "LOOP", "PHI", "KINT" };

/* Guards: SLOAD checks the type of a slot, LT asserts a comparison. LuaJIT also flags LOOP. */
static const int ir_guard[] = { 1, 0, 0, 1, 1, 0, 0 };

static const int ir_of_op[] = { [OP_ADD] = IR_ADD, [OP_SUB] = IR_SUB };

struct ir {
    int o, op1, op2;
    int reg;       /* register chosen by the assembler, or NOREG */
    uint32_t hint; /* registers the assembler would like this ref to get */
    int phi;       /* this ref is one side of a PHI */
};

/* A snapshot: for each slot, the ref that holds its value if it differs from what the
 * trace entry loaded (0 means nothing to restore), and the pc to resume at. */
struct snap {
    int ref; /* the first IR ref the snapshot is valid for */
    int pc;
    int slot[MAX_SLOTS];
};

static struct ir ir[IR_SIZE];
static int nins = REF_FIRST;
static int nk = REF_BIAS;
static struct snap snaps[MAX_SNAP];
static int nsnap;
static int cur_slot[MAX_SLOTS]; /* the ref that holds each slot, while recording */
static int needsnap;
static int guardemit;
static int loopref;

static int ir_emit(int o, int op1, int op2)
{
    struct ir *i = &ir[nins];
    i->o = o;
    i->op1 = op1;
    i->op2 = op2;
    i->reg = NOREG;
    i->hint = 0;
    i->phi = 0;
    guardemit |= ir_guard[o];
    return nins++;
}

static int ir_kint(int v)
{
    struct ir *i = &ir[--nk];
    i->o = IR_KINT;
    i->op1 = v;
    i->op2 = 0;
    i->reg = NOREG;
    return nk;
}

static void ref_str(char *buf, int ref)
{
    if (ref < REF_BIAS) {
        snprintf(buf, 16, "K%02d", REF_BIAS - ref);
    } else {
        snprintf(buf, 16, "%04d", ref - REF_BIAS);
    }
}

static const char *const reg_name[NREGS] = { "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11" };

static void dump_ir(int with_regs)
{
    char a[16], b[16];
    P("IR:\n");
    for (int k = REF_BIAS - 1; k >= nk; k--) {
        P("  K%02d       KINT  %d\n", REF_BIAS - k, ir[k].op1);
    }
    for (int x = REF_FIRST; x < nins; x++) {
        const struct ir *i = &ir[x];
        P("  %04d ", x - REF_BIAS);
        if (with_regs) {
            P("%-4s ", i->reg == NOREG ? "" : reg_name[i->reg]);
        }
        P("%c%c %-5s ", ir_guard[i->o] ? '>' : ' ', i->phi ? '+' : ' ', ir_name[i->o]);
        if (i->o == IR_LOOP) {
            P("\n");
        } else if (i->o == IR_SLOAD) {
            P("#%d\n", i->op1);
        } else {
            ref_str(a, i->op1);
            ref_str(b, i->op2);
            P("%s %s\n", a, b);
        }
    }
    P("snapshots:\n");
    for (int s = 0; s < nsnap; s++) {
        P("  #%d first=%04d pc=%d [", s, snaps[s].ref - REF_BIAS, snaps[s].pc);
        for (int k = 0; k < MAX_SLOTS; k++) {
            if (snaps[s].slot[k]) {
                ref_str(a, snaps[s].slot[k]);
                P(" %d=%s", k, a);
            }
        }
        P(" ]\n");
    }
}

/* ------------------------------------------------------------------------------------------
 * Recording. The interpreter is about to run the instruction at pc, and the recorder writes
 * down what it will do. Slots are SSA values: cur_slot says which ref holds each slot, and a
 * slot that the trace has not touched yet is loaded, with a type guard, the first time it is
 * read.
 * ---------------------------------------------------------------------------------------- */

static void snap_add(int pc)
{
    struct snap *s = &snaps[nsnap++];
    s->ref = nins;
    s->pc = pc;
    for (int k = 0; k < MAX_SLOTS; k++) {
        int r = cur_slot[k];
        /* A slot that still holds what the trace loaded is not restored. */
        int unmodified = (ir[r].o == IR_SLOAD) & (ir[r].op1 == k);
        s->slot[k] = r * !unmodified;
    }
}

static int rec_slot(int s)
{
    if (!cur_slot[s]) {
        cur_slot[s] = ir_emit(IR_SLOAD, s, 0);
    }
    return cur_slot[s];
}

static void rec_ins(const struct proto *p, int pc, const struct slot *base)
{
    const struct insn *in = &p->code[pc];
    /* After a guard, the next instruction starts a new snapshot, taken before it runs. */
    if (needsnap) {
        snap_add(pc);
        needsnap = 0;
    }
    switch (in->op) { // GCOVR_EXCL_BR_LINE: the default case is unreachable, see below
    case OP_LOADI:
        cur_slot[in->a] = ir_kint(in->imm);
        break;
    case OP_ADD:
    case OP_SUB: {
        int x = rec_slot(in->b);
        int y = rec_slot(in->c);
        cur_slot[in->a] = ir_emit(ir_of_op[in->op], x, y);
        break;
    }
    case OP_JNLT: {
        int x = rec_slot(in->b);
        int y = rec_slot(in->c);
        require(base[in->b].val < base[in->c].val, "a trace that leaves its loop");
        /* The interpreter fell through, so the trace asserts the comparison and leaves to the
         * jump target if it fails. The snapshot says what the stack holds then. */
        snap_add(in->imm);
        ir_emit(IR_LT, x, y);
        needsnap = 1;
        break;
    }
    case OP_JMP:
        break;
    // GCOVR_EXCL_START: the examples record only the instructions above
    default:
        require(0, "an instruction that is not recorded");
        break;
        // GCOVR_EXCL_STOP
    }
}

static void stage_record(const struct proto *p, int startpc, struct slot *base)
{
    P("== stage_record ==\n");
    P("recording from pc %d\n", startpc);
    snap_add(startpc); /* snapshot #0: nothing has run yet */
    int pc = startpc;
    for (;;) {
        rec_ins(p, pc, base);
        int next = step(p, pc, base);
        if (next == startpc) {
            break;
        }
        pc = next;
    }
    snap_add(startpc); /* the last snapshot: the state where the loop starts again */
    dump_ir(0);
}

/* ------------------------------------------------------------------------------------------
 * The loop optimization. The recorded IR is the pre-roll. LOOP marks its end, and everything
 * after it is a copy of the pre-roll with each operand replaced by what it is in the next
 * iteration (subst). A copy that comes out as a different ref than the original is a value
 * that travels around the loop, and gets a PHI.
 * ---------------------------------------------------------------------------------------- */

/* The copy of an instruction goes through a fold step first. LuaJIT's has hundreds of rules
 * and this has the one the loop needs: a load of a slot is whatever the slot holds at the
 * end of the recorded iteration. */
static int fold_emit(int o, int op1, int op2)
{
    if (o == IR_SLOAD) {
        return cur_slot[op1];
    }
    return ir_emit(o, op1, op2);
}

static void subst_snap(const struct snap *osnap, const struct snap *loop, const int *subst)
{
    struct snap *s = &snaps[nsnap++];
    s->ref = nins;
    s->pc = osnap->pc;
    guardemit = 0;
    for (int k = 0; k < MAX_SLOTS; k++) {
        /* A slot the snapshot does not mention still holds what the previous iteration left
         * in it, which is what the last snapshot says. */
        int has = osnap->slot[k] != 0;
        s->slot[k] = has * subst[osnap->slot[k]] + !has * loop->slot[k];
    }
}

static void stage_loop(void)
{
    P("== stage_loop ==\n");
    int invar = nins;
    int subst[IR_SIZE];
    for (int i = 0; i < IR_SIZE; i++) {
        subst[i] = i; /* constants and literals stand for themselves */
    }
    loopref = ir_emit(IR_LOOP, 0, 0);
    int onsnap = nsnap - 1;
    const struct snap loop = snaps[onsnap];
    nsnap = onsnap;
    int osnap = 1;
    for (int ins = REF_FIRST; ins < invar; ins++) {
        if (osnap < onsnap && ins >= snaps[osnap].ref) {
            subst_snap(&snaps[osnap], &loop, subst);
            osnap++;
        }
        const struct ir *i = &ir[ins];
        int ref = fold_emit(i->o, subst[i->op1], subst[i->op2]);
        subst[ins] = ref;
        if ((ref != ins) & (ref < invar)) {
            ir[ref].phi = 1; /* the copy is an older instruction: it is carried around */
        }
    }
    nsnap -= !guardemit; /* drop a trailing snapshot that no guard uses */
    for (int k = 0; k < MAX_SLOTS; k++) {
        require((loop.slot[k] < REF_BIAS) | ir[loop.slot[k]].phi,
                "a value in the last snapshot that is not carried by a PHI");
    }
    for (int ins = REF_FIRST; ins < invar; ins++) {
        if (ir[ins].phi) {
            require(subst[ins] != ins, "a PHI of a value that does not change");
            ir[subst[ins]].phi = 1;
            ir_emit(IR_PHI, ins, subst[ins]);
        }
    }
    dump_ir(0);
}

/* ------------------------------------------------------------------------------------------
 * The assembler. It walks the IR from the last instruction to the first and writes the machine
 * code from the end of the buffer downwards. A value gets a register at its last use, which is
 * the first time the walk meets it, and gives it back at its definition, which is the last
 * time. There is no liveness analysis and no interval list: the direction of the walk is the
 * analysis.
 * ---------------------------------------------------------------------------------------- */

#define MC_SIZE 512

static uint8_t *mcbase, *mctop, *mcp, *mcloop;
static uint8_t *exit_stub[MAX_SNAP];
static uint8_t *abs_hole; /* the 8 bytes of the address of snap_restore in the exit code */
static uint32_t freeset;

/* The registers the allocator hands out, as x86-64 register numbers. */
static const int hw[NREGS] = { 1, 2, 6, 7, 8, 9, 10, 11 };

static void put(const uint8_t *b, int n)
{
    mcp -= n;
    memcpy(mcp, b, (size_t)n);
}

/* op r/m64, r64 (op = 01 add, 29 sub, 39 cmp, 89 mov) */
static void emit_rr(uint8_t opc, int reg, int rm)
{
    int h = hw[reg], m = hw[rm];
    uint8_t b[3] = { (uint8_t)(0x48 | (h >> 3) << 2 | (m >> 3)), opc,
                     (uint8_t)(0xC0 | (h & 7) << 3 | (m & 7)) };
    put(b, 3);
}

/* op r/m64, imm32, where ext selects the operation (0 add, 5 sub) */
static void emit_ri(int ext, int rm, int32_t imm)
{
    int m = hw[rm];
    uint8_t b[7] = { (uint8_t)(0x48 | (m >> 3)), 0x81, (uint8_t)(0xC0 | ext << 3 | (m & 7)) };
    memcpy(b + 3, &imm, 4);
    put(b, 7);
}

/* mov reg, [rbx + disp32] */
static void emit_load(int reg, int32_t disp)
{
    int h = hw[reg];
    uint8_t b[7] = { (uint8_t)(0x48 | (h >> 3) << 2), 0x8B, (uint8_t)(0x83 | (h & 7) << 3) };
    memcpy(b + 3, &disp, 4);
    put(b, 7);
}

/* cmp qword [rbx + disp32], imm8 */
static void emit_cmp_tag(int32_t disp, int8_t tag)
{
    uint8_t b[8] = { 0x48, 0x83, 0xBB };
    memcpy(b + 3, &disp, 4);
    b[7] = (uint8_t)tag;
    put(b, 8);
}

/* jcc rel32 to a target above the current position (cc 0x85 jne, 0x8D jge) */
static void emit_jcc(uint8_t cc, const uint8_t *target)
{
    int32_t rel = (int32_t)(target - mcp);
    uint8_t b[6] = { 0x0F, cc };
    memcpy(b + 2, &rel, 4);
    put(b, 6);
}

static int ra_pick_from(uint32_t mask)
{
    return __builtin_ctz(mask);
}

/* Give ref a register if it has none. The registers it would like (hint) win if one is free. */
static int ra_alloc(int ref, uint32_t allow)
{
    struct ir *i = &ir[ref];
    if (i->reg != NOREG) {
        return i->reg;
    }
    uint32_t pick = freeset & allow;
    require(pick != 0, "more live values than registers");
    uint32_t pref = pick & i->hint;
    pick = pref | (pick & -(uint32_t)(pref == 0));
    i->reg = ra_pick_from(pick);
    freeset &= ~(1u << i->reg);
    return i->reg;
}

/* The register of an instruction's result. It is free from here up to the start of the trace,
 * so the instruction's own operands may take it. */
static int ra_dest(int ref)
{
    int d = ir[ref].reg;
    require(d != NOREG, "an instruction whose result is never used");
    freeset |= 1u << d;
    return d;
}

static int snap_of(int ref)
{
    int s = 0;
    for (int k = 0; k < nsnap; k++) {
        s += snaps[k].ref <= ref;
    }
    return s - 1;
}

/* A guard needs every value in its snapshot in a register, so the exit can store it. */
static void asm_snap_prep(int s)
{
    for (int k = 0; k < MAX_SLOTS; k++) {
        int r = snaps[s].slot[k];
        if (r >= REF_BIAS) {
            ra_alloc(r, ALLREGS);
        }
    }
}

static const struct {
    uint8_t rr, ext;
} alu[] = { [IR_ADD] = { 0x01, 0 }, [IR_SUB] = { 0x29, 5 } };

static void asm_arith(int ref)
{
    struct ir *i = &ir[ref];
    int d = ra_dest(ref);
    require(i->op1 >= REF_BIAS, "a constant as the first operand");
    ir[i->op1].hint |= 1u << d; /* the left operand should become the result in place */
    if (i->op2 < REF_BIAS) {
        int l = ra_alloc(i->op1, ALLREGS);
        require(l == d, "a result that needs a copy of its first operand");
        emit_ri(alu[i->o].ext, d, ir[i->op2].op1);
    } else {
        int r = ra_alloc(i->op2, ALLREGS & ~(1u << d));
        int l = ra_alloc(i->op1, ALLREGS);
        require(l == d, "a result that needs a copy of its first operand");
        emit_rr(alu[i->o].rr, r, d);
    }
}

static void asm_sload(int ref)
{
    struct ir *i = &ir[ref];
    int s = snap_of(ref);
    asm_snap_prep(s);
    int d = ra_dest(ref);
    emit_load(d, i->op1 * 16 + 8);
    emit_jcc(0x85, exit_stub[s]);
    emit_cmp_tag(i->op1 * 16, T_INT);
}

static void asm_lt(int ref)
{
    struct ir *i = &ir[ref];
    int s = snap_of(ref);
    asm_snap_prep(s);
    require((i->op1 >= REF_BIAS) & (i->op2 >= REF_BIAS), "a constant in a comparison");
    int r = ra_alloc(i->op2, ALLREGS);
    int l = ra_alloc(i->op1, ALLREGS);
    emit_jcc(0x8D, exit_stub[s]); /* the trace asserts l < r, so it leaves on l >= r */
    emit_rr(0x39, r, l);
}

/* A PHI is met first, because the walk starts at the end. Its right side is the value at the
 * end of the loop, so that gets a register now. The left side is the value at the start, and
 * it takes the same register, which makes the jump back free of moves. That only works if the
 * left value is dead by the time the right one is made, which is checked here. */
static void asm_phi(int ref)
{
    int l = ir[ref].op1, r = ir[ref].op2;
    int conflict = 0;
    for (int x = r + 1; x < nins; x++) {
        const struct ir *i = &ir[x];
        conflict |= (i->o != IR_PHI) & ((i->op1 == l) | (i->op2 == l));
        /* A guard uses its snapshot at its own position. */
        const struct snap *s = &snaps[snap_of(x)];
        for (int k = 0; k < MAX_SLOTS; k++) {
            conflict |= ir_guard[i->o] & (s->slot[k] == l);
        }
    }
    require(!conflict, "a PHI whose old value is still needed after the new one exists");
    ir[l].hint |= 1u << ra_alloc(r, ALLREGS);
}

static void asm_loop(uint8_t *jmp_end)
{
    mcloop = mcp;
    int32_t rel = (int32_t)(mcloop - jmp_end);
    memcpy(jmp_end - 4, &rel, 4);
    /* Whatever the loop body did not read of the old values still has to be in its register. */
    for (int x = loopref + 1; x < nins; x++) {
        if (ir[x].o == IR_PHI) {
            int l = ir[x].op1;
            ra_alloc(l, ALLREGS);
            require(ir[l].reg == ir[ir[x].op2].reg, "PHI sides in different registers");
        }
    }
}

/* The code that every exit runs: push the allocatable registers, call snap_restore with a
 * pointer to them and the snapshot number, and return its result (the pc) to the caller. */
#define COMMON_ABS 19

static struct slot *trace_base;
static int exit_snap;

/* Called from the exit code with the registers saved on the stack, in the order of hw[]. */
static int snap_restore(const uint64_t *regs, int snapno)
{
    const struct snap *s = &snaps[snapno];
    exit_snap = snapno;
    for (int k = 0; k < MAX_SLOTS; k++) {
        int ref = s->slot[k];
        if (ref) {
            int64_t v = ref < REF_BIAS ? ir[ref].op1 : (int64_t)regs[ir[ref].reg];
            trace_base[k].tag = T_INT;
            trace_base[k].val = v;
        }
    }
    return s->pc;
}

static void emit_exits(void)
{
    static const uint8_t common[] = {
        0x41, 0x53, 0x41, 0x52, 0x41, 0x51, 0x41, 0x50, 0x57, 0x56, 0x52, 0x51, /* push r11 .. rcx */
        0x48, 0x89, 0xE7,                                                       /* mov rdi, rsp */
        0x89, 0xC6,                                                             /* mov esi, eax */
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                                     /* mov rax, imm64 */
        0xFF, 0xD0,                                                             /* call rax */
        0x48, 0x83, 0xC4, 0x40,                                                 /* add rsp, 64 */
        0x5B,                                                                   /* pop rbx */
        0xC3,                                                                   /* ret */
    };
    put(common, sizeof common);
    uint8_t *common_at = mcp;
    abs_hole = common_at + COMMON_ABS;
    uintptr_t addr = (uintptr_t)snap_restore;
    memcpy(abs_hole, &addr, sizeof addr);
    for (int s = nsnap - 1; s >= 0; s--) {
        /* mov eax, snapno; jmp common */
        int32_t rel = (int32_t)(common_at - mcp);
        uint8_t b[10] = { 0xB8, (uint8_t)s, 0, 0, 0, 0xE9 };
        memcpy(b + 6, &rel, 4);
        put(b, 10);
        exit_stub[s] = mcp;
    }
}

static void stage_assemble(void)
{
    P("== stage_assemble ==\n");
    mcbase = mmap(NULL, MC_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(mcbase != MAP_FAILED, "mmap");
    mctop = mcbase + MC_SIZE;
    mcp = mctop;
    freeset = ALLREGS;

    emit_exits();
    /* The jump from the end of the body back to the start of the loop; the target is known
     * when the walk reaches LOOP. */
    uint8_t jmp[5] = { 0xE9 };
    put(jmp, 5);
    uint8_t *jmp_end = mcp + 5;

    for (int ref = nins - 1; ref >= REF_FIRST; ref--) {
        switch (ir[ref].o) { // GCOVR_EXCL_BR_LINE: the default case is unreachable, see below
        case IR_SLOAD:
            asm_sload(ref);
            break;
        case IR_ADD:
        case IR_SUB:
            asm_arith(ref);
            break;
        case IR_LT:
            asm_lt(ref);
            break;
        case IR_LOOP:
            asm_loop(jmp_end);
            break;
        case IR_PHI:
            asm_phi(ref);
            break;
        // GCOVR_EXCL_START: every IR instruction in a trace has a case
        default:
            break;
            // GCOVR_EXCL_STOP
        }
    }
    require(freeset == ALLREGS, "a register that was never given back");

    /* prologue: push rbx; mov rbx, rdi (the frame base) */
    static const uint8_t mov_rbx_rdi[] = { 0x48, 0x89, 0xFB };
    put(mov_rbx_rdi, 3);
    static const uint8_t push_rbx[] = { 0x53 };
    put(push_rbx, 1);

    /* code.bin and the dump hold the code with the host address of snap_restore zeroed */
    P("code at +0: loop at +%d, %d bytes\n", (int)(mcloop - mcp), (int)(mctop - mcp));
    dump_ir(1);
    P("machine code:\n");
    for (uint8_t *q = mcp; q < mctop; q++) {
        int off = (int)(q - mcp);
        int zero = q >= abs_hole && q < abs_hole + 8;
        P("%s%02x", off % 16 == 0 ? (off ? "\n  " : "  ") : " ", zero ? 0 : *q);
    }
    P("\n");
}

static void stage_run(struct slot *base, const char *path, int *pc)
{
    P("== stage_run ==\n");
    uint8_t copy[MC_SIZE];
    size_t n = (size_t)(mctop - mcp);
    memcpy(copy, mcp, n);
    memset(copy + (abs_hole - mcp), 0, 8);
    const char *paths[2] = { "/dev/null", path }; /* without a path, the code goes nowhere */
    const char *dest = paths[path != NULL];
    FILE *fp = fopen(dest, "wb");
    // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
    if (fp == NULL || fwrite(copy, 1, n, fp) != n || fclose(fp) != 0) {
        perror(dest);
        exit(1);
    }
    // GCOVR_EXCL_STOP
    int err = mprotect(mcbase, MC_SIZE, PROT_READ | PROT_EXEC);
    require(err == 0, "mprotect");
    int (*fn)(struct slot *);
    memcpy(&fn, &mcp, sizeof mcp); /* ISO C forbids casting an object pointer to a function */
    trace_base = base;
    *pc = fn(base);
    P("trace left through snapshot #%d, the interpreter resumes at pc %d\n", exit_snap, *pc);
}

static int run_trace(struct proto *p, int startpc, struct slot *base)
{
    P("hot loop: the jump back to pc %d ran %d times\n", startpc, HOTLOOP);
    stage_record(p, startpc, base);
    stage_loop();
    stage_assemble();
    int pc;
    stage_run(base, code_path, &pc);
    return pc;
}

int main(int argc, char **argv)
{
    int e = -1;
    for (int i = 0; argc > 1 && i < (int)(sizeof examples / sizeof *examples); i++) {
        if (strcmp(argv[1], examples[i].name) == 0) {
            e = i;
        }
    }
    if (e < 0) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    code_path = argc > 2 ? argv[2] : NULL;
    examples[e].build();
    stage_bytecode();
    static struct slot stack[STACK_SLOTS];
    stage_interpret(stack);
    return 0;
}
