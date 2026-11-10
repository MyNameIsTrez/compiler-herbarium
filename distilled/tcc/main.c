/*
 * tcc: a cut-down version of the Tiny C Compiler, for x86-64.
 *
 * TCC has no syntax tree and no IR. The parser calls code generation functions as it goes,
 * and they work on a value stack (vstack) whose entries describe where a value currently is:
 * a constant, a local variable in memory, a register, the CPU flags, or a symbol. Nothing is
 * loaded until an operation needs it in a register (gv). A comparison leaves its result in the
 * flags (VT_CMP) and the next conditional jump consumes it. Forward jumps are chains threaded
 * through the unfinished rel32 fields in the code, which gsym_addr patches when the target is
 * known. The function prologue is patched after the body, once the frame size is known.
 *
 * This entry skips the tokenizer: the C tokens of each example are written down by hand. The
 * parser, value stack, instruction selection and jump chains are the real design in small.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout, the intermediate form after each stage on stderr,
 *   and writes the machine code to code.bin if a path is given.
 *
 * Stages:
 *   stage_source   pick the C token stream of the example (stands in for the tokenizer)
 *   stage_codegen  one pass over the tokens: every vstack action and every instruction emitted
 *   stage_export   stands in for the linker and loader: write the bytes, map them and run main
 */
#define _DEFAULT_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------------------------------
 * Tokens. Like TCC, single characters stand for themselves and the rest are numbered above 256.
 * TOK_LT has the value of the setl condition code, which TCC uses as the comparison operator.
 * ---------------------------------------------------------------------------------------- */

enum {
    TOK_LT = 0x9c,
    TOK_ID = 0x100,
    TOK_NUM,
    TOK_STR,
    TOK_INT,
    TOK_VOID,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_RETURN,
    TOK_EOF,
};

struct token {
    int type;
    const char *text;
};

#define T(type, text) {type, text}
#define LP T('(', "(")
#define RP T(')', ")")
#define LB T('{', "{")
#define RB T('}', "}")
#define SEMI T(';', ";")
#define PLUS T('+', "+")
#define MINUS T('-', "-")
#define STAR T('*', "*")
#define EQ T('=', "=")
#define LT T(TOK_LT, "<")
#define ID(s) T(TOK_ID, s)
#define NUM(s) T(TOK_NUM, s)
#define INT T(TOK_INT, "int")
#define VOID T(TOK_VOID, "void")
#define IF T(TOK_IF, "if")
#define ELSE T(TOK_ELSE, "else")
#define WHILE T(TOK_WHILE, "while")
#define RETURN T(TOK_RETURN, "return")
#define EOT T(TOK_EOF, "")
#define MAIN INT, ID("main"), LP, VOID, RP, LB
#define PRINT_INT ID("print_int"), LP

/* int main(void) { print_str("Hello, world!\n"); } */
static const struct token hello_tokens[] = {
    MAIN, ID("print_str"), LP, T(TOK_STR, "Hello, world!\n"), RP, SEMI, RB, EOT,
};

/* int main(void) { print_int((2 + 3) * 4 - 6); } */
static const struct token arithmetic_tokens[] = {
    MAIN, PRINT_INT, LP, NUM("2"), PLUS, NUM("3"), RP, STAR, NUM("4"), MINUS, NUM("6"), RP, SEMI,
    RB, EOT,
};

/* int main(void) { int a = 3; int b = 5; if (a < b) print_int(b); else print_int(a); } */
static const struct token branch_tokens[] = {
    MAIN,
    INT, ID("a"), EQ, NUM("3"), SEMI,
    INT, ID("b"), EQ, NUM("5"), SEMI,
    IF, LP, ID("a"), LT, ID("b"), RP,
    PRINT_INT, ID("b"), RP, SEMI,
    ELSE,
    PRINT_INT, ID("a"), RP, SEMI,
    RB, EOT,
};

/* int main(void) { int sum = 0; int i = 1; while (i < 11) { sum = sum + i; i = i + 1; }
 *                  print_int(sum); } */
static const struct token loop_tokens[] = {
    MAIN,
    INT, ID("sum"), EQ, NUM("0"), SEMI,
    INT, ID("i"), EQ, NUM("1"), SEMI,
    WHILE, LP, ID("i"), LT, NUM("11"), RP,
    LB,
    ID("sum"), EQ, ID("sum"), PLUS, ID("i"), SEMI,
    ID("i"), EQ, ID("i"), PLUS, NUM("1"), SEMI,
    RB,
    PRINT_INT, ID("sum"), RP, SEMI,
    RB, EOT,
};

/* int fact(int n) { if (n < 2) { return 1; } return n * fact(n - 1); }
 * int main(void) { print_int(fact(5)); } */
static const struct token call_tokens[] = {
    INT, ID("fact"), LP, INT, ID("n"), RP, LB,
    IF, LP, ID("n"), LT, NUM("2"), RP,
    LB, RETURN, NUM("1"), SEMI, RB,
    RETURN, ID("n"), STAR, ID("fact"), LP, ID("n"), MINUS, NUM("1"), RP, SEMI,
    RB,
    MAIN, PRINT_INT, ID("fact"), LP, NUM("5"), RP, RP, SEMI, RB, EOT,
};

struct source {
    const char *name;
    const struct token *tokens;
    size_t count;
};

#define SOURCE(n) {#n, n##_tokens, sizeof(n##_tokens) / sizeof(n##_tokens[0])}
static const struct source sources[] = {
    SOURCE(hello), SOURCE(arithmetic), SOURCE(branch), SOURCE(loop), SOURCE(call),
};

/* ------------------------------------------------------------------------------------------
 * Emitting bytes, from tccgen.c and x86_64-gen.c: o, g, gen_le32, oad
 *
 * Every instruction is logged with its offset, its bytes and a text that the caller set
 * with say() just before.
 * ---------------------------------------------------------------------------------------- */

#define CODE_SIZE 4096
static uint8_t code[CODE_SIZE];
static int ind; /* offset where the next byte goes */
static char txt[64];

#define say(...) snprintf(txt, sizeof txt, __VA_ARGS__)

static void emit(const uint8_t *bytes, size_t n)
{
    char hex[48];
    for (size_t i = 0; i < n; i++) {
        snprintf(&hex[3 * i], 4, "%02x ", bytes[i]);
        code[ind + (int)i] = bytes[i];
    }
    fprintf(stderr, "    +0x%04x  %-24s %s\n", ind, hex, txt);
    ind += (int)n;
}

#define INSN(...)                                                                                  \
    do {                                                                                           \
        const uint8_t bytes_[] = {__VA_ARGS__};                                                    \
        emit(bytes_, sizeof bytes_);                                                               \
    } while (0)

static uint32_t read32le(int at)
{
    return (uint32_t)code[at] | (uint32_t)code[at + 1] << 8 | (uint32_t)code[at + 2] << 16 |
           (uint32_t)code[at + 3] << 24;
}

static void write32le(int at, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        code[at + i] = (uint8_t)(v >> (8 * i));
    }
}

/* An instruction whose last four bytes are a rel32 field holding `t`. Returns the offset of
 * that field, which is what a chain of unresolved references links through. */
static int oad(const uint8_t *prefix, size_t n, int t)
{
    uint8_t bytes[8];
    memcpy(bytes, prefix, n);
    for (int i = 0; i < 4; i++) {
        bytes[n + (size_t)i] = (uint8_t)((uint32_t)t >> (8 * i));
    }
    int field = ind + (int)n;
    emit(bytes, n + 4);
    return field;
}

/* ------------------------------------------------------------------------------------------
 * Symbols: functions and the strings of string literals
 * ---------------------------------------------------------------------------------------- */

struct sym {
    char name[16];
    const char *text; /* the bytes of a string, NULL for a function */
    int addr;         /* offset in the code, or -1 until a string is placed */
    int chain;        /* unresolved rip-relative references to this symbol */
};

static struct sym syms[8];
static int nsyms;

static int new_sym(const char *name, const char *text, int addr)
{
    snprintf(syms[nsyms].name, sizeof syms[nsyms].name, "%s", name);
    syms[nsyms].text = text;
    syms[nsyms].addr = addr;
    return nsyms++;
}

static int find_sym(const char *name)
{
    int i = 0;
    while (strcmp(syms[i].name, name) != 0) {
        i++;
    }
    return i;
}

/* ------------------------------------------------------------------------------------------
 * The value stack, from tcc.h and tccgen.c
 *
 * r holds a location (the low bits) and flags. The locations are a register number, VT_CONST,
 * VT_LOCAL and VT_CMP. VT_LVAL says that the value is in memory at that location, VT_SYM that
 * a constant is the address of a symbol.
 * ---------------------------------------------------------------------------------------- */

#define VT_VALMASK 0x003f
#define VT_CONST 0x0030 /* constant in c, the first location that is not a register */
#define VT_LOCAL 0x0032 /* offset c from rbp */
#define VT_CMP 0x0033   /* the value is in the processor flags, the condition is cmp_op */
#define VT_LVAL 0x0100
#define VT_SYM 0x0200

#define REG_RAX 0
#define REG_RCX 1
#define REG_RDX 2
#define REG_RSI 6
#define REG_RDI 7

struct svalue {
    int r;
    int c; /* the constant, the rbp offset, or the symbol index */
    int cmp_op;
};

static struct svalue vstack[9]; /* entry 0 is a sentinel, so vtop[-1] is always valid */
static struct svalue *vtop = vstack;

static const char *const r32[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
static const char *const r64[8] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"};

static void vtrace(void)
{
    fprintf(stderr, "    vstack:");
    for (const struct svalue *p = vstack + 1; p <= vtop; p++) {
        if (p->r == VT_CMP) {
            fprintf(stderr, " [flags: <]");
        } else if ((p->r & VT_VALMASK) == VT_LOCAL) {
            fprintf(stderr, " [local %d]", p->c);
        } else if ((p->r & VT_VALMASK) == VT_CONST) {
            if (p->r & VT_SYM) {
                fprintf(stderr, " [%s]", syms[p->c].name);
            } else {
                fprintf(stderr, " [%d]", p->c);
            }
        } else {
            fprintf(stderr, " [%s]", r64[p->r]);
        }
    }
    fprintf(stderr, "\n");
}

static void vsetc(int r, int c)
{
    vtop++;
    vtop->r = r;
    vtop->c = c;
    vtop->cmp_op = 0;
}

static void vpushi(int v)
{
    fprintf(stderr, "  vpushi %d\n", v);
    vsetc(VT_CONST, v);
    vtrace();
}

static void vpush_sym(int sym)
{
    fprintf(stderr, "  vpush_sym %s\n", syms[sym].name);
    vsetc(VT_CONST | VT_SYM, sym);
    vtrace();
}

static void vpush_local(int offset)
{
    fprintf(stderr, "  vpush local %d\n", offset);
    vsetc(VT_LOCAL | VT_LVAL, offset);
    vtrace();
}

static void vswap(void)
{
    struct svalue tmp = vtop[0];
    vtop[0] = vtop[-1];
    vtop[-1] = tmp;
}

static void vpop(void)
{
    vtop--;
}

/* ------------------------------------------------------------------------------------------
 * Loading a value into a register: load() in x86_64-gen.c, gv() and get_reg() in tccgen.c
 * ---------------------------------------------------------------------------------------- */

/* Emit the instruction that brings the value `sv` into register r */
static void load(int r, const struct svalue *sv)
{
    int v = sv->r & VT_VALMASK;
    if (sv->r & VT_LVAL) {
        /* mov r, [rbp+disp8]: locals stay within 128 bytes of the frame pointer */
        say("mov %s, [rbp%+d]", r32[r], sv->c);
        INSN(0x8b, (uint8_t)(0x45 | (r << 3)), (uint8_t)sv->c);
    } else if (v != VT_CONST) {
        say("mov %s, %s", r32[r], r32[v]);
        INSN(0x89, (uint8_t)(0xc0 + r + v * 8));
    } else if (sv->r & VT_SYM) {
        /* lea r, [rip+rel32]: the string is placed after the code, so the field starts a chain */
        say("lea %s, [rip+%s]", r64[r], syms[sv->c].name);
        const uint8_t prefix[] = {0x48, 0x8d, (uint8_t)(0x05 + r * 8)};
        syms[sv->c].chain = oad(prefix, sizeof prefix, syms[sv->c].chain);
    } else {
        say("mov %s, %d", r32[r], sv->c);
        const uint8_t prefix[] = {(uint8_t)(0xb8 + r)};
        oad(prefix, sizeof prefix, sv->c);
    }
}

/* Store register r into the lvalue `sv` (a local) */
static void store(int r, const struct svalue *sv)
{
    say("mov [rbp%+d], %s", sv->c, r32[r]);
    INSN(0x89, (uint8_t)(0x45 | (r << 3)), (uint8_t)sv->c);
}

/* The first register that no entry of the value stack lives in. TCC would spill one to a
 * temporary local when none is free, and none of the examples needs more than two. */
static int get_reg(void)
{
    static const int regs[] = {REG_RAX, REG_RCX, REG_RDX, REG_RSI, REG_RDI};
    for (int i = 0;; i++) {
        bool used = false;
        for (const struct svalue *p = vstack + 1; p <= vtop; p++) {
            used = used || p->r == regs[i];
        }
        if (!used) {
            return regs[i];
        }
    }
}

/* Make vtop a plain register value, loading it only if it is not one already */
static int gv(void)
{
    int r = vtop->r;
    /* Anything that is not already a plain register: a constant, a symbol, or memory */
    if (r >= VT_CONST) {
        r = get_reg();
        load(r, vtop);
    }
    vtop->r = r;
    return r;
}

/* Put vtop[-1] and vtop in registers. The one below goes first, so that get_reg gives the one
 * above a different register. */
static void gv2(void)
{
    vswap();
    gv();
    vswap();
    gv();
}

/* ------------------------------------------------------------------------------------------
 * Jumps: gjmp, gjmp_cond, gjmp_addr, gsym and gsym_addr
 *
 * A forward jump has its rel32 field filled with the previous jump to the same target, and the
 * offset of the field is the new head of the chain. 0 ends a chain, so the code never starts
 * a chain at offset 0.
 * ---------------------------------------------------------------------------------------- */

static int gjmp(int t)
{
    say("jmp <forward, chain +0x%x>", t);
    const uint8_t prefix[] = {0xe9};
    return oad(prefix, sizeof prefix, t);
}

static int gjmp_cond(int op, int t)
{
    say("jge <forward, chain +0x%x>", t); /* the only condition is <, always jumped on when false */
    const uint8_t prefix[] = {0x0f, (uint8_t)(op - 16)};
    return oad(prefix, sizeof prefix, t);
}

/* A backward jump to a known address, in its short form */
static void gjmp_addr(int a)
{
    say("jmp +0x%x", a);
    INSN(0xeb, (uint8_t)(a - ind - 2));
}

/* Resolve every jump of the chain t to address a */
static void gsym_addr(int t, int a)
{
    while (t) {
        int next = (int)read32le(t);
        write32le(t, (uint32_t)(a - t - 4));
        fprintf(stderr, "    patch +0x%04x: rel32 %d, target +0x%04x\n", t, a - t - 4, a);
        t = next;
    }
}

/* Resolve the chain t to here, if there is one */
static void gsym(int t)
{
    if (t) {
        gsym_addr(t, ind);
    }
}

/* ------------------------------------------------------------------------------------------
 * Operations: gen_op and gen_opi, vstore, gvtst, gfunc_call
 * ---------------------------------------------------------------------------------------- */

static const char *opname(int op)
{
    switch (op) {
    case '+':
        return "+";
    case '-':
        return "-";
    case '*':
        return "*";
    default:
        return "<";
    }
}

/*
 * Integer binary operation. add, sub and cmp have the same two encodings, picked by opc: with
 * a constant right operand, `op reg, imm8` (the left operand is loaded first, the constant
 * never is), and otherwise `op reg, reg`. A comparison emits cmp and leaves the result in the
 * flags: vtop becomes VT_CMP and no register is used for the answer.
 */
static void gen_op(int op)
{
    static const char *const mnemonic[] = {"add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"};
    fprintf(stderr, "  gen_op %s\n", opname(op));
    if (op == '*') {
        gv2();
        int r = vtop[-1].r;
        int fr = vtop[0].r;
        say("imul %s, %s", r32[r], r32[fr]);
        INSN(0x0f, 0xaf, (uint8_t)(0xc0 + fr + r * 8));
        vtop--;
    } else {
        int opc = op == '+' ? 0 : op == '-' ? 5 : 7;
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            vswap();
            int r = gv();
            vswap();
            say("%s %s, %d", mnemonic[opc], r32[r], vtop->c);
            INSN(0x83, (uint8_t)(0xc0 | (opc << 3) | r), (uint8_t)vtop->c);
        } else {
            gv2();
            int r = vtop[-1].r;
            int fr = vtop[0].r;
            say("%s %s, %s", mnemonic[opc], r32[r], r32[fr]);
            INSN((uint8_t)((opc << 3) | 1), (uint8_t)(0xc0 + r + fr * 8));
        }
        vtop--;
        if (op == TOK_LT) {
            vtop->r = VT_CMP;
            vtop->cmp_op = op;
        }
    }
    vtrace();
}

/* Assignment: the lvalue is below the value. The value stays on the stack as the result. */
static void vstore(void)
{
    fprintf(stderr, "  vstore\n");
    int r = gv();
    store(r, vtop - 1);
    vswap();
    vtop--;
    vtrace();
}

/*
 * Turn the flags on top of the stack into a conditional jump to chain t, taken when the
 * condition is false (TCC's inv = 1, the only form the examples need). The inverse of a
 * condition code is the code with its lowest bit flipped. The jump is added to the chain.
 */
static int gvtst(int t)
{
    fprintf(stderr, "  gvtst\n");
    t = gjmp_cond(vtop->cmp_op ^ 1, t);
    vtop--;
    vtrace();
    return t;
}

/* A call with one argument: the function symbol is below it. The result is in rax. */
static void gfunc_call(void)
{
    fprintf(stderr, "  gfunc_call\n");
    int r = gv();
    say("mov rdi, %s", r64[r]);
    INSN(0x48, 0x89, (uint8_t)(0xc0 + r * 8 + REG_RDI));
    vtop--;
    int fn = vtop->c;
    vtop--;
    /* The functions are all placed before they are called, so the rel32 is known at once */
    say("call %s", syms[fn].name);
    const uint8_t prefix[] = {0xe8};
    oad(prefix, sizeof prefix, syms[fn].addr - (ind + 5));
    vsetc(REG_RAX, 0);
    vtrace();
}

/* Leave the value on top of the stack in rax and drop it */
static void gfunc_return(void)
{
    fprintf(stderr, "  gfunc_return\n");
    int r = gv();
    if (r != REG_RAX) {
        load(REG_RAX, vtop);
    }
    vtop--;
}

/* ------------------------------------------------------------------------------------------
 * Parser, from tccgen.c: unary, expr_infix, expr_eq, block
 *
 * It calls the functions above as it recognizes the source and builds nothing in between.
 * ---------------------------------------------------------------------------------------- */

static const struct token *tokp;
static struct token tok;

static void next(void)
{
    tok = *tokp++;
}

/* The tokens are well formed, so the expected one is simply skipped over */
static void skip(int type)
{
    (void)type;
    next();
}

struct local {
    const char *name;
    int off;
};

static struct local locals[8];
static int nlocals;
static int loc;                /* rbp offset of the lowest local so far */
static int rsym;               /* chain of the jumps from return statements to the epilogue */
static int func_sub_sp_offset; /* where the prologue ends, so where the body starts */
static int local_scope;
static const char *funcname;

static int add_local(const char *name, int size)
{
    loc = (loc - size) & -size;
    locals[nlocals].name = name;
    locals[nlocals].off = loc;
    return locals[nlocals++].off;
}

static int find_local(const char *name)
{
    int i = 0;
    while (strcmp(locals[i].name, name) != 0) {
        i++;
    }
    return locals[i].off;
}

static void gexpr(void);

/* A name followed by a parenthesis is a call with one argument, otherwise a local variable */
static void unary(void)
{
    struct token t = tok;
    next();
    switch (t.type) {
    case TOK_NUM:
        vpushi(atoi(t.text));
        break;
    case TOK_STR: {
        char name[16];
        snprintf(name, sizeof name, ".LC%d", nsyms);
        vpush_sym(new_sym(name, t.text, -1));
        break;
    }
    case '(':
        gexpr();
        skip(')');
        break;
    default: /* TOK_ID */
        if (tok.type == '(') {
            vpush_sym(find_sym(t.text));
            next();
            gexpr();
            skip(')');
            gfunc_call();
        } else {
            vpush_local(find_local(t.text));
        }
        break;
    }
}

static int precedence(int t)
{
    switch (t) {
    case '*':
        return 11;
    case '+':
    case '-':
        return 10;
    case TOK_LT:
        return 8;
    default:
        return 0;
    }
}

/* Precedence climbing: operators of precedence p or higher, each with its right operand */
static void expr_infix(int p)
{
    int t = tok.type;
    int p2;
    while ((p2 = precedence(t)) >= p) {
        next();
        unary();
        expr_infix(p2 + 1);
        gen_op(t);
        t = tok.type;
    }
}

static void gexpr(void)
{
    unary();
    expr_infix(1);
    if (tok.type == '=') {
        next();
        gexpr();
        vstore();
    }
}

/* main returns 0 when control reaches the end of its body */
static void check_func_return(void)
{
    if (strcmp(funcname, "main") == 0) {
        vpushi(0);
        gfunc_return();
    }
}

static void block(void)
{
    if (tok.type == TOK_IF) {
        next();
        skip('(');
        gexpr();
        int a = gvtst(0);
        skip(')');
        block();
        if (tok.type == TOK_ELSE) {
            int d = gjmp(0);
            gsym(a);
            next();
            block();
            gsym(d);
        } else {
            gsym(a);
        }
    } else if (tok.type == TOK_WHILE) {
        next();
        int d = ind;
        skip('(');
        gexpr();
        int a = gvtst(0);
        skip(')');
        block();
        gjmp_addr(d);
        gsym(a);
    } else if (tok.type == '{') {
        local_scope++;
        next();
        while (tok.type != '}') {
            block();
        }
        local_scope--;
        if (local_scope) {
            next();
        } else {
            check_func_return();
        }
    } else if (tok.type == TOK_RETURN) {
        next();
        gexpr();
        gfunc_return();
        skip(';');
        /* No jump after the last statement of the function body. TCC also jumps when another
         * statement follows at the top level, which no example has. */
        if (local_scope != 1) {
            rsym = gjmp(rsym);
        }
    } else if (tok.type == TOK_INT) {
        next();
        const char *name = tok.text;
        next();
        vpush_local(add_local(name, 4));
        skip('=');
        gexpr();
        vstore();
        vpop();
        skip(';');
    } else {
        gexpr();
        vpop();
        skip(';');
    }
}

/* ------------------------------------------------------------------------------------------
 * Functions: gen_function, gfunc_prolog and gfunc_epilog
 * ---------------------------------------------------------------------------------------- */

#define FUNC_PROLOG_SIZE 11 /* push rbp; mov rbp, rsp; sub rsp, imm32 */

static void gfunc_epilog(void)
{
    say("leave");
    INSN(0xc9);
    say("ret");
    INSN(0xc3);

    /* The frame size is known now: go back and fill in the prologue that was left blank */
    int frame = (-loc + 15) & -16;
    int start = func_sub_sp_offset - FUNC_PROLOG_SIZE;
    const uint8_t prolog[] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec};
    memcpy(&code[start], prolog, sizeof prolog);
    write32le(start + (int)sizeof prolog, (uint32_t)frame);
    fprintf(stderr, "    patch +0x%04x: push rbp; mov rbp, rsp; sub rsp, %d\n", start, frame);
}

static void gen_function(void)
{
    next(); /* int */
    funcname = tok.text;
    next();
    new_sym(funcname, NULL, ind);
    fprintf(stderr, "== function %s at +0x%04x ==\n", funcname, ind);

    ind += FUNC_PROLOG_SIZE;
    func_sub_sp_offset = ind;
    loc = 0;
    nlocals = 0;
    rsym = 0;
    local_scope = 0;
    skip('(');
    if (tok.type == TOK_INT) {
        /* the one parameter arrives in rdi and is stored in a local */
        next();
        int off = add_local(tok.text, 8);
        next();
        say("mov [rbp%+d], rdi", off);
        INSN(0x48, 0x89, 0x7d, (uint8_t)off);
    } else {
        next(); /* void */
    }
    skip(')');
    block();
    next(); /* the closing brace of the body */
    gsym(rsym);
    gfunc_epilog();
}

/* ------------------------------------------------------------------------------------------
 * Runtime. A real program calls print_int and print_str in libc. They are hand-assembled here
 * (write system calls) and placed first, so every call to them is to a known address.
 * ---------------------------------------------------------------------------------------- */

static const uint8_t print_int_code[] = {
    0x48, 0x89, 0xf8, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x74, 0x24, 0x20,
    0x48, 0xc7, 0xc1, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xff, 0xce, 0xc6, 0x06, 0x0a,
    0x31, 0xd2, 0x48, 0xf7, 0xf1, 0x80, 0xc2, 0x30, 0x48, 0xff, 0xce, 0x88, 0x16,
    0x48, 0x85, 0xc0, 0x75, 0xee,
    0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0x29, 0xf2, 0xb8, 0x01, 0x00, 0x00, 0x00,
    0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x20, 0xc3,
};

static const uint8_t print_str_code[] = {
    0x48, 0x89, 0xfe, 0x31, 0xd2, 0x80, 0x3c, 0x17, 0x00, 0x74, 0x05, 0x48, 0xff, 0xc2,
    0xeb, 0xf5, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05,
    0xc3,
};

static void place_runtime(const char *name, const uint8_t *bytes, size_t n)
{
    new_sym(name, NULL, ind);
    memcpy(&code[ind], bytes, n);
    fprintf(stderr, "  runtime %s at +0x%04x, %zu bytes\n", name, ind, n);
    ind += (int)n;
}

/* ------------------------------------------------------------------------------------------
 * Stages
 * ---------------------------------------------------------------------------------------- */

static const struct source *stage_source(const char *name)
{
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        if (strcmp(sources[i].name, name) == 0) {
            fprintf(stderr, "== stage_source: C tokens of %s ==\n", name);
            for (size_t t = 0; t + 1 < sources[i].count; t++) {
                fprintf(stderr, "%s%s", t == 0 ? "" : " ", sources[i].tokens[t].text);
            }
            fprintf(stderr, "\n");
            return &sources[i];
        }
    }
    return NULL;
}

static void stage_codegen(const struct source *source)
{
    fprintf(stderr, "== stage_codegen: each vstack action, with the instructions it emits ==\n");
    place_runtime("print_int", print_int_code, sizeof print_int_code);
    place_runtime("print_str", print_str_code, sizeof print_str_code);

    tokp = source->tokens;
    next();
    while (tok.type != TOK_EOF) {
        gen_function();
    }

    /* The string literals go after the code. Now their address is known. */
    for (int i = 0; i < nsyms; i++) {
        if (syms[i].text != NULL) {
            syms[i].addr = ind;
            size_t n = strlen(syms[i].text) + 1;
            memcpy(&code[ind], syms[i].text, n);
            fprintf(stderr, "== data %s at +0x%04x, %zu bytes ==\n", syms[i].name, ind, n);
            ind += (int)n;
            gsym_addr(syms[i].chain, syms[i].addr);
        }
    }
}

static void stage_export(const char *path)
{
    fprintf(stderr, "== stage_export: final code, %d bytes ==", ind);
    for (int i = 0; i < ind; i++) {
        if (i % 16 == 0) {
            fprintf(stderr, "\n+0x%04x:", i);
        }
        fprintf(stderr, " %02x", code[i]);
    }
    fprintf(stderr, "\n");

    if (path != NULL) {
        FILE *fp = fopen(path, "wb");
        // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
        if (fp == NULL || fwrite(code, 1, (size_t)ind, fp) != (size_t)ind || fclose(fp) != 0) {
            perror(path);
            exit(1);
        }
        // GCOVR_EXCL_STOP
    }
    void *buf = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (buf == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    memcpy(buf, code, (size_t)ind);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (mprotect(buf, CODE_SIZE, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    /* ISO C forbids casting an object pointer to a function pointer, so copy the bits. */
    void *entry = (uint8_t *)buf + syms[find_sym("main")].addr;
    void (*fn)(void);
    memcpy(&fn, &entry, sizeof fn);
    fn();
}

int main(int argc, char **argv)
{
    const struct source *source = NULL;
    if (argc < 2 || (source = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_codegen(source);
    stage_export(argc > 2 ? argv[2] : NULL);
    return 0;
}
