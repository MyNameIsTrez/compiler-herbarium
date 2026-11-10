/*
 * chibicc: a cut-down version of Rui Ueyama's chibicc, a small C compiler for x86-64.
 *
 * chibicc has no IR. The parser builds a typed AST, a pass gives every local variable a place
 * on the stack, and the code generator walks the tree once, writing a fixed run of assembly
 * for each node. Every expression leaves its value in rax. A binary operation evaluates its
 * right side, pushes it, evaluates its left side and pops the right side into rdi. There is no
 * register allocation at all: the machine stack is the only temporary storage.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout, the intermediate form after each stage on stderr,
 *   and writes the machine code to code.bin if a path is given.
 *
 * Stages, in the order of the real pipeline:
 *   stage_ast              parse.c and type.c: the typed AST, with locals and the hidden objects
 *   stage_assign_offsets   codegen.c assign_lvar_offsets: a frame offset for every local
 *   stage_codegen          codegen.c gen_expr and gen_stmt: assembly text, node by node
 *   stage_encode           stands in for the assembler (chibicc runs `as`): bytes and rel32 fields
 *   stage_export           stands in for the linker and the loader: map the bytes and run main
 */
#define _DEFAULT_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

#define MAX_OBJS 24
#define MAX_NODES 160
#define MAX_INSNS 300
#define MAX_LABELS 64
#define NAME 24 /* longest name, with the NUL */
#define SYM 40  /* longest label: ".L.return." and a name */
#define HELLO_STRING "Hello, world!\n"

/* Only the patterns that the examples need are written. A shape that none of them contains
 * stops the program here. */
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
 * stage_ast
 *
 * chibicc's parser turns tokens into Node and Obj records. Here the examples are hardcoded as
 * the calls that the parser would make, with the same helpers (new_lvar, new_gvar,
 * new_string_literal, new_unique_name) and the same quirks:
 *  - Locals and globals are prepended to their lists, so the lists run from newest to oldest.
 *  - Every function gets a hidden local, __alloca_size__, and two string literals for
 *    __func__ and __FUNCTION__, which only use up names (.L..0, .L..1, ...).
 *  - `int x = e;` becomes a comma of a ND_MEMZERO and an assignment.
 *  - `while` is a ND_FOR with only a condition and a body.
 *  - add_type runs on every statement of a block, and sets the type of each node.
 * ---------------------------------------------------------------------------------------- */

enum { TY_INT, TY_PTR, TY_ARRAY, TY_FUNC };
struct type {
    int kind, size, align;
    const struct type *return_ty;
};
static const struct type ty_int = {TY_INT, 4, 4, NULL};
static const struct type ty_ptr = {TY_PTR, 8, 8, NULL};
static const struct type ty_func = {TY_FUNC, 1, 1, &ty_int}; /* every function returns int */

enum {
    ND_NUM, ND_VAR, ND_ASSIGN, ND_ADD, ND_SUB, ND_MUL, ND_LT, ND_COMMA, ND_MEMZERO, ND_FUNCALL,
    ND_IF, ND_FOR, ND_RETURN, ND_EXPR_STMT, ND_BLOCK,
};
static const char *const node_names[] = {
    "num", "var", "assign", "add", "sub", "mul", "lt", "comma", "memzero", "funcall",
    "if", "for", "return", "expr_stmt", "block",
};

struct node {
    int kind;
    struct node *next, *lhs, *rhs, *cond, *then, *els, *body, *args;
    int64_t val;
    struct obj *var;
    const struct type *ty;
    char brk_label[NAME], cont_label[NAME];
};

struct obj {
    char name[NAME];
    const struct type *ty;
    struct type array_ty; /* storage for the type of a string literal */
    int is_local, is_function, is_definition;
    int offset, align;
    struct obj *next;
    struct node *body;
    struct obj *params, *locals, *alloca_bottom;
    int stack_size;
    const char *init_data;
};

static struct obj objs[MAX_OBJS];
static int nobjs;
static struct node nodes[MAX_NODES];
static int nnodes;
static struct obj *globals; /* chibicc calls this list prog */
static struct obj *current_fn;

static void new_unique_name(char *out)
{
    static int id = 0;
    snprintf(out, NAME, ".L..%d", id++);
}

static struct obj *new_var(const char *name, const struct type *ty)
{
    require(nobjs < MAX_OBJS, "more objects");
    struct obj *var = &objs[nobjs++];
    snprintf(var->name, NAME, "%s", name);
    var->ty = ty;
    var->align = ty->align;
    return var;
}

static struct obj *new_lvar(const char *name, const struct type *ty)
{
    struct obj *var = new_var(name, ty);
    var->is_local = 1;
    var->next = current_fn->locals;
    current_fn->locals = var;
    return var;
}

static struct obj *new_gvar(const char *name, const struct type *ty)
{
    struct obj *var = new_var(name, ty);
    var->next = globals;
    globals = var;
    return var;
}

static struct obj *new_string_literal(const char *data)
{
    char name[NAME];
    new_unique_name(name);
    struct obj *var = new_gvar(name, &ty_ptr);
    var->array_ty = (struct type){TY_ARRAY, (int)strlen(data) + 1, 1, NULL};
    var->ty = &var->array_ty;
    var->align = 1;
    var->init_data = data;
    return var;
}

/* A prototype: `int name(int);` */
static struct obj *declare_function(const char *name)
{
    struct obj *fn = new_gvar(name, &ty_func);
    fn->is_function = 1;
    return fn;
}

/* The start of a definition, as in parse.c function(): the parameters are locals, params is
 * the list of locals at that moment, then come the hidden objects. */
static void begin_function(struct obj *fn, const char *param)
{
    current_fn = fn;
    fn->is_definition = 1;
    if (param != NULL) {
        new_lvar(param, &ty_int);
    }
    fn->params = fn->locals;
    fn->alloca_bottom = new_lvar("__alloca_size__", &ty_ptr);
    new_string_literal(fn->name); /* __func__ */
    new_string_literal(fn->name); /* __FUNCTION__ */
}

static struct node *new_node(int kind)
{
    require(nnodes < MAX_NODES, "more nodes");
    struct node *node = &nodes[nnodes++];
    node->kind = kind;
    return node;
}

static void add_type(struct node *node)
{
    if (node == NULL || node->ty != NULL) {
        return;
    }
    add_type(node->lhs);
    add_type(node->rhs);
    add_type(node->cond);
    add_type(node->then);
    add_type(node->els);
    for (struct node *n = node->body; n != NULL; n = n->next) {
        add_type(n);
    }
    /* A call is typed when it is built, so the walk never reaches its arguments. */
    switch (node->kind) {
    case ND_NUM:
        node->ty = &ty_int;
        return;
    case ND_ADD:
    case ND_SUB:
    case ND_MUL:
    case ND_ASSIGN:
        node->ty = node->lhs->ty;
        return;
    case ND_LT:
        node->ty = &ty_int;
        return;
    case ND_VAR:
        node->ty = node->var->ty;
        return;
    case ND_COMMA:
        node->ty = node->rhs->ty;
        return;
    }
}

static struct node *new_num(int64_t val)
{
    struct node *node = new_node(ND_NUM);
    node->val = val;
    return node;
}

static struct node *new_var_node(struct obj *var)
{
    struct node *node = new_node(ND_VAR);
    node->var = var;
    return node;
}

static struct node *new_binary(int kind, struct node *lhs, struct node *rhs)
{
    struct node *node = new_node(kind);
    node->lhs = lhs;
    node->rhs = rhs;
    return node;
}

static struct node *new_unary(int kind, struct node *lhs)
{
    struct node *node = new_node(kind);
    node->lhs = lhs;
    return node;
}

#define NIL ((struct node *)0)

/* Links its arguments through next. The list ends at NIL. */
static struct node *chain(struct node *first, ...)
{
    va_list ap;
    va_start(ap, first);
    struct node *cur = first;
    struct node *n = va_arg(ap, struct node *);
    while (n != NULL) {
        cur->next = n;
        cur = n;
        n = va_arg(ap, struct node *);
    }
    va_end(ap);
    return first;
}

/* compound_stmt: a block types each statement as soon as it is complete. */
static struct node *new_block(struct node *body)
{
    struct node *node = new_node(ND_BLOCK);
    node->body = body;
    for (struct node *n = body; n != NULL; n = n->next) {
        add_type(n);
    }
    return node;
}

static struct node *funcall(struct obj *fn, struct node *args)
{
    struct node *callee = new_var_node(fn);
    add_type(callee);
    for (struct node *arg = args; arg != NULL; arg = arg->next) {
        add_type(arg);
    }
    struct node *node = new_unary(ND_FUNCALL, callee);
    node->args = args;
    node->ty = callee->ty->return_ty;
    return node;
}

static struct node *expr_stmt(struct node *expr) { return new_unary(ND_EXPR_STMT, expr); }

/* `int name = init;` declares the local, then clears it and assigns to it. */
static struct node *declaration(const char *name, struct node *init)
{
    struct obj *var = new_lvar(name, &ty_int);
    struct node *zero = new_node(ND_MEMZERO);
    zero->var = var;
    struct node *assign = new_binary(ND_ASSIGN, new_var_node(var), init);
    return expr_stmt(new_binary(ND_COMMA, zero, assign));
}

static struct node *new_if(struct node *cond, struct node *then, struct node *els)
{
    struct node *node = new_node(ND_IF);
    node->cond = cond;
    node->then = then;
    node->els = els;
    return node;
}

static struct node *new_while(struct node *cond, struct node *then)
{
    struct node *node = new_node(ND_FOR);
    node->cond = cond;
    node->then = then;
    new_unique_name(node->brk_label);
    new_unique_name(node->cont_label);
    return node;
}

static struct obj *print_int_fn, *print_str_fn;

static void declare_runtime(void)
{
    print_int_fn = declare_function("print_int");
    print_str_fn = declare_function("print_str");
}

static void build_hello(void)
{
    declare_runtime();
    struct obj *main_fn = declare_function("main");
    begin_function(main_fn, NULL);
    struct obj *text = new_string_literal(HELLO_STRING);
    main_fn->body = new_block(expr_stmt(funcall(print_str_fn, new_var_node(text))));
}

static void build_arithmetic(void)
{
    declare_runtime();
    struct obj *main_fn = declare_function("main");
    begin_function(main_fn, NULL);
    /* print_int((2 + 3) * 4 - 6); */
    struct node *sum = new_binary(ND_ADD, new_num(2), new_num(3));
    struct node *product = new_binary(ND_MUL, sum, new_num(4));
    struct node *difference = new_binary(ND_SUB, product, new_num(6));
    main_fn->body = new_block(expr_stmt(funcall(print_int_fn, difference)));
}

static void build_branch(void)
{
    declare_runtime();
    struct obj *main_fn = declare_function("main");
    begin_function(main_fn, NULL);
    /* int a = 3; int b = 5; if (a < b) print_int(b); else print_int(a); */
    struct node *decl_a = declaration("a", new_num(3));
    struct node *decl_b = declaration("b", new_num(5));
    struct obj *a = main_fn->locals->next;
    struct obj *b = main_fn->locals;
    struct node *cond = new_binary(ND_LT, new_var_node(a), new_var_node(b));
    struct node *then = expr_stmt(funcall(print_int_fn, new_var_node(b)));
    struct node *els = expr_stmt(funcall(print_int_fn, new_var_node(a)));
    main_fn->body = new_block(chain(decl_a, decl_b, new_if(cond, then, els), NIL));
}

static void build_loop(void)
{
    declare_runtime();
    struct obj *main_fn = declare_function("main");
    begin_function(main_fn, NULL);
    /* int sum = 0; int i = 1; while (i < 11) { sum = sum + i; i = i + 1; } print_int(sum); */
    struct node *decl_sum = declaration("sum", new_num(0));
    struct node *decl_i = declaration("i", new_num(1));
    struct obj *sum = main_fn->locals->next;
    struct obj *i = main_fn->locals;
    struct node *cond = new_binary(ND_LT, new_var_node(i), new_num(11));
    struct node *add_sum = new_binary(ND_ADD, new_var_node(sum), new_var_node(i));
    struct node *set_sum = expr_stmt(new_binary(ND_ASSIGN, new_var_node(sum), add_sum));
    struct node *add_i = new_binary(ND_ADD, new_var_node(i), new_num(1));
    struct node *set_i = expr_stmt(new_binary(ND_ASSIGN, new_var_node(i), add_i));
    struct node *body = new_block(chain(set_sum, set_i, NIL));
    struct node *print = expr_stmt(funcall(print_int_fn, new_var_node(sum)));
    main_fn->body = new_block(chain(decl_sum, decl_i, new_while(cond, body), print, NIL));
}

static void build_call(void)
{
    declare_runtime();
    /* int fact(int n) { if (n < 2) return 1; return n * fact(n - 1); } */
    struct obj *fact_fn = declare_function("fact");
    begin_function(fact_fn, "n");
    struct obj *n = fact_fn->params;
    struct node *cond = new_binary(ND_LT, new_var_node(n), new_num(2));
    struct node *base = new_unary(ND_RETURN, new_num(1));
    struct node *minus = new_binary(ND_SUB, new_var_node(n), new_num(1));
    struct node *rec = funcall(fact_fn, minus);
    struct node *times = new_binary(ND_MUL, new_var_node(n), rec);
    fact_fn->body = new_block(chain(new_if(cond, base, NULL), new_unary(ND_RETURN, times), NIL));
    /* int main(void) { print_int(fact(5)); } */
    struct obj *main_fn = declare_function("main");
    begin_function(main_fn, NULL);
    struct node *five = funcall(fact_fn, new_num(5));
    main_fn->body = new_block(expr_stmt(funcall(print_int_fn, five)));
}

struct program {
    const char *name;
    void (*build)(void);
};

static const struct program programs[] = {
    {"hello", build_hello}, {"arithmetic", build_arithmetic}, {"branch", build_branch},
    {"loop", build_loop},   {"call", build_call},
};

static const struct program *find_program(const char *name)
{
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        if (strcmp(programs[i].name, name) == 0) {
            return &programs[i];
        }
    }
    return NULL;
}

static const char *type_name(const struct type *ty, char *buf)
{
    switch (ty->kind) {
    case TY_INT:
        return "int";
    case TY_PTR:
        return "char*";
    case TY_ARRAY:
        snprintf(buf, NAME, "char[%d]", ty->size);
        return buf;
    }
    return "func"; /* TY_FUNC */
}

static void print_node(const char *role, const struct node *node, int level)
{
    if (node == NULL) {
        return;
    }
    char buf[NAME];
    P("%*s%s%s%s", 2 * level, "", role, role[0] != '\0' ? " " : "", node_names[node->kind]);
    if (node->kind == ND_NUM) {
        P(" %ld", (long)node->val);
    }
    if ((node->kind == ND_VAR) | (node->kind == ND_MEMZERO)) {
        P(" %s", node->var->name);
    }
    if (node->kind == ND_FOR) {
        P(" (brk %s, cont %s)", node->brk_label, node->cont_label);
    }
    if (node->ty != NULL) {
        P(" : %s", type_name(node->ty, buf));
    }
    P("\n");
    print_node("lhs", node->lhs, level + 1);
    print_node("rhs", node->rhs, level + 1);
    print_node("cond", node->cond, level + 1);
    print_node("then", node->then, level + 1);
    print_node("else", node->els, level + 1);
    for (const struct node *n = node->body; n != NULL; n = n->next) {
        print_node("", n, level + 1);
    }
    for (const struct node *n = node->args; n != NULL; n = n->next) {
        print_node("arg", n, level + 1);
    }
}

static void stage_ast(const struct program *program)
{
    program->build();
    P("== stage_ast: typed AST ==\n");
    for (const struct obj *fn = globals; fn != NULL; fn = fn->next) {
        if (fn->is_function & fn->is_definition) {
            P("function %s\n", fn->name);
            print_node("", fn->body, 1);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_assign_offsets
 *
 * Walks the locals of each function from the newest to the oldest. Each one gets the next
 * free slot below rbp, aligned to its own alignment, and the frame is rounded up to 16 bytes.
 * Parameters are locals too: the prologue copies them from their registers into their slots.
 * ---------------------------------------------------------------------------------------- */

static int align_to(int n, int align) { return (n + align - 1) / align * align; }

static void stage_assign_offsets(void)
{
    P("== stage_assign_offsets: frame layout ==\n");
    for (struct obj *fn = globals; fn != NULL; fn = fn->next) {
        if (!fn->is_function) {
            continue;
        }
        int bottom = 0;
        for (struct obj *var = fn->locals; var != NULL; var = var->next) {
            bottom += var->ty->size;
            bottom = align_to(bottom, var->align);
            var->offset = -bottom;
        }
        fn->stack_size = align_to(bottom, 16);
        if (fn->is_definition) {
            P("%s: stack_size %d\n", fn->name, fn->stack_size);
            for (const struct obj *var = fn->locals; var != NULL; var = var->next) {
                char buf[NAME];
                P("  %-16s %-6s size %d  offset %d\n", var->name, type_name(var->ty, buf),
                  var->ty->size, var->offset);
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_codegen
 *
 * gen_expr and gen_stmt walk the tree and append instructions. They are kept in a list, and
 * printed in the syntax chibicc writes for `as`. `depth` counts the pushes that are not yet
 * popped, which decides the stack alignment at a call.
 * ---------------------------------------------------------------------------------------- */

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10 };
static const char *const reg64[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                    "rsi", "rdi", "r8",  "r9",  "r10"};
static const char *const reg32[] = {"eax", "ecx", "edx", "ebx", "esp",
                                    "ebp", "esi", "edi", "r8d", "r9d"};
static const int argreg[] = {RDI, RSI, RDX, RCX, R8, R9};

enum {
    A_GLOBL, A_LABEL, A_PUSH, A_POP, A_MOV_RI, A_MOV_RR, A_LEA_RBP, A_LEA_RIP, A_LOAD32S,
    A_STORE32, A_STORE_RBP32, A_STORE_RBP64, A_ADD, A_SUB, A_IMUL, A_CMP, A_CMP0, A_SETL,
    A_MOVZB, A_JE, A_JMP, A_CALL_R10, A_ADD_RSP, A_SUB_RSP, A_MOV_AL0, A_REP_STOSB, A_RET,
};

struct insn {
    int op;
    int r1, r2;
    int64_t imm;
    char sym[SYM];
};

static struct insn code[MAX_INSNS];
static int ncode;
static int depth;

static void ins(int op, int r1, int r2, int64_t imm, const char *sym)
{
    require(ncode < MAX_INSNS, "more instructions");
    struct insn *in = &code[ncode++];
    in->op = op;
    in->r1 = r1;
    in->r2 = r2;
    in->imm = imm;
    snprintf(in->sym, SYM, "%s", sym != NULL ? sym : "");
}

static void label(const char *name) { ins(A_LABEL, 0, 0, 0, name); }

static void push(void)
{
    ins(A_PUSH, RAX, 0, 0, NULL);
    depth++;
}

static void pop(int reg)
{
    ins(A_POP, reg, 0, 0, NULL);
    depth--;
}

static int count(void)
{
    static int i = 1;
    return i++;
}

/* The address of a variable goes to rax. A function or a global is addressed relative to rip,
 * which is what chibicc does for a symbol that is defined in the same file. */
static void gen_addr(const struct node *node)
{
    if (node->var->is_local) {
        ins(A_LEA_RBP, RAX, 0, node->var->offset, NULL);
    } else {
        ins(A_LEA_RIP, RAX, 0, 0, node->var->name);
    }
}

/* An array or a function stays an address. An int is loaded and sign-extended to 64 bits. */
static void load(const struct type *ty)
{
    if ((ty->kind == TY_ARRAY) | (ty->kind == TY_FUNC)) {
        return;
    }
    ins(A_LOAD32S, 0, 0, 0, NULL);
}

/* The address was pushed before the value was computed. */
static void store(void)
{
    pop(RDI);
    ins(A_STORE32, 0, 0, 0, NULL);
}

static void cmp_zero(void) { ins(A_CMP0, 0, 0, 0, NULL); }

static void gen_expr(const struct node *node);

/* Arguments are evaluated and pushed from the last to the first. */
static void push_args(const struct node *args)
{
    if (args == NULL) {
        return;
    }
    push_args(args->next);
    gen_expr(args);
    push();
}

static void gen_expr(const struct node *node)
{
    switch (node->kind) {
    case ND_NUM:
        ins(A_MOV_RI, RAX, 0, node->val, NULL);
        return;
    case ND_VAR:
        gen_addr(node);
        load(node->ty);
        return;
    case ND_ASSIGN:
        gen_addr(node->lhs);
        push();
        gen_expr(node->rhs);
        store();
        return;
    case ND_COMMA:
        gen_expr(node->lhs);
        gen_expr(node->rhs);
        return;
    case ND_MEMZERO:
        /* `rep stosb` is memset(rdi, al, rcx). */
        ins(A_MOV_RI, RCX, 0, node->var->ty->size, NULL);
        ins(A_LEA_RBP, RDI, 0, node->var->offset, NULL);
        ins(A_MOV_AL0, 0, 0, 0, NULL);
        ins(A_REP_STOSB, 0, 0, 0, NULL);
        return;
    case ND_FUNCALL: {
        /* chibicc pads the stack with `sub $8, %rsp` when depth is odd. Every call in the
         * examples happens at an even depth. */
        require(depth % 2 == 0, "a call at an odd stack depth");
        push_args(node->args);
        gen_expr(node->lhs);
        int gp = 0;
        for (const struct node *arg = node->args; arg != NULL; arg = arg->next) {
            pop(argreg[gp++]);
        }
        ins(A_MOV_RR, RAX, R10, 0, NULL);
        ins(A_MOV_RI, RAX, 0, 0, NULL); /* the number of vector registers used */
        ins(A_CALL_R10, 0, 0, 0, NULL);
        ins(A_ADD_RSP, 0, 0, 0, NULL); /* 8 bytes for each argument passed on the stack: none */
        return;
    }
    }

    gen_expr(node->rhs);
    push();
    gen_expr(node->lhs);
    pop(RDI);

    switch (node->kind) {
    case ND_ADD:
        ins(A_ADD, 0, 0, 0, NULL);
        return;
    case ND_SUB:
        ins(A_SUB, 0, 0, 0, NULL);
        return;
    case ND_MUL:
        ins(A_IMUL, 0, 0, 0, NULL);
        return;
    default: /* ND_LT */
        ins(A_CMP, 0, 0, 0, NULL);
        ins(A_SETL, 0, 0, 0, NULL);
        ins(A_MOVZB, 0, 0, 0, NULL);
        return;
    }
}

static void gen_stmt(const struct node *node)
{
    char buf[SYM], buf2[SYM];
    switch (node->kind) {
    case ND_IF: {
        const int c = count();
        gen_expr(node->cond);
        cmp_zero();
        snprintf(buf, SYM, ".L.else.%d", c);
        snprintf(buf2, SYM, ".L.end.%d", c);
        ins(A_JE, 0, 0, 0, buf);
        gen_stmt(node->then);
        ins(A_JMP, 0, 0, 0, buf2);
        label(buf);
        if (node->els != NULL) {
            gen_stmt(node->els);
        }
        label(buf2);
        return;
    }
    case ND_FOR: {
        const int c = count();
        snprintf(buf, SYM, ".L.begin.%d", c);
        label(buf);
        gen_expr(node->cond);
        cmp_zero();
        ins(A_JE, 0, 0, 0, node->brk_label);
        gen_stmt(node->then);
        label(node->cont_label);
        ins(A_JMP, 0, 0, 0, buf);
        label(node->brk_label);
        return;
    }
    case ND_RETURN:
        gen_expr(node->lhs);
        snprintf(buf, SYM, ".L.return.%s", current_fn->name);
        ins(A_JMP, 0, 0, 0, buf);
        return;
    case ND_EXPR_STMT:
        gen_expr(node->lhs);
        return;
    }
    require(node->kind == ND_BLOCK, "this statement");
    for (const struct node *n = node->body; n != NULL; n = n->next) {
        gen_stmt(n);
    }
}

static void emit_text(void)
{
    for (struct obj *fn = globals; fn != NULL; fn = fn->next) {
        if (!fn->is_function || !fn->is_definition) {
            continue;
        }
        char ret[SYM];
        ins(A_GLOBL, 0, 0, 0, fn->name);
        label(fn->name);
        current_fn = fn;

        ins(A_PUSH, RBP, 0, 0, NULL);
        ins(A_MOV_RR, RSP, RBP, 0, NULL);
        ins(A_SUB_RSP, 0, 0, fn->stack_size, NULL);
        ins(A_STORE_RBP64, RSP, 0, fn->alloca_bottom->offset, NULL);

        /* Parameters arrive in registers and are saved into their slots. */
        int gp = 0;
        for (const struct obj *var = fn->params; var != NULL; var = var->next) {
            ins(A_STORE_RBP32, argreg[gp++], 0, var->offset, NULL);
        }

        gen_stmt(fn->body);
        require(depth == 0, "a statement that leaves the stack unbalanced");

        /* Reaching the end of main is the same as returning 0. */
        if (strcmp(fn->name, "main") == 0) {
            ins(A_MOV_RI, RAX, 0, 0, NULL);
        }

        snprintf(ret, SYM, ".L.return.%s", fn->name);
        label(ret);
        ins(A_MOV_RR, RBP, RSP, 0, NULL);
        ins(A_POP, RBP, 0, 0, NULL);
        ins(A_RET, 0, 0, 0, NULL);
    }
}

/* One instruction in the syntax of `as`, without the indentation. */
static void fmt_insn(char *out, size_t size, const struct insn *in)
{
    const char *r1 = reg64[in->r1];
    const char *r2 = reg64[in->r2];
    const long imm = (long)in->imm;
    switch (in->op) {
    case A_GLOBL:
        snprintf(out, size, ".globl %s", in->sym);
        break;
    case A_LABEL:
        snprintf(out, size, "%s:", in->sym);
        break;
    case A_PUSH:
        snprintf(out, size, "push %%%s", r1);
        break;
    case A_POP:
        snprintf(out, size, "pop %%%s", r1);
        break;
    case A_MOV_RI:
        snprintf(out, size, "mov $%ld, %%%s", imm, r1);
        break;
    case A_MOV_RR:
        snprintf(out, size, "mov %%%s, %%%s", r1, r2);
        break;
    case A_LEA_RBP:
        snprintf(out, size, "lea %ld(%%rbp), %%%s", imm, r1);
        break;
    case A_LEA_RIP:
        snprintf(out, size, "lea %s(%%rip), %%rax", in->sym);
        break;
    case A_LOAD32S:
        snprintf(out, size, "movsxd (%%rax), %%rax");
        break;
    case A_STORE32:
        snprintf(out, size, "mov %%eax, (%%rdi)");
        break;
    case A_STORE_RBP32:
        snprintf(out, size, "mov %%%s, %ld(%%rbp)", reg32[in->r1], imm);
        break;
    case A_STORE_RBP64:
        snprintf(out, size, "mov %%%s, %ld(%%rbp)", r1, imm);
        break;
    case A_ADD:
        snprintf(out, size, "add %%edi, %%eax");
        break;
    case A_SUB:
        snprintf(out, size, "sub %%edi, %%eax");
        break;
    case A_IMUL:
        snprintf(out, size, "imul %%edi, %%eax");
        break;
    case A_CMP:
        snprintf(out, size, "cmp %%edi, %%eax");
        break;
    case A_CMP0:
        snprintf(out, size, "cmp $0, %%eax");
        break;
    case A_SETL:
        snprintf(out, size, "setl %%al");
        break;
    case A_MOVZB:
        snprintf(out, size, "movzb %%al, %%rax");
        break;
    case A_JE:
        snprintf(out, size, "je %s", in->sym);
        break;
    case A_JMP:
        snprintf(out, size, "jmp %s", in->sym);
        break;
    case A_CALL_R10:
        snprintf(out, size, "call *%%r10");
        break;
    case A_ADD_RSP:
        snprintf(out, size, "add $%ld, %%rsp", imm);
        break;
    case A_SUB_RSP:
        snprintf(out, size, "sub $%ld, %%rsp", imm);
        break;
    case A_MOV_AL0:
        snprintf(out, size, "mov $0, %%al");
        break;
    case A_REP_STOSB:
        snprintf(out, size, "rep stosb");
        break;
    default: /* A_RET */
        snprintf(out, size, "ret");
        break;
    }
}

/* The string literals, in the order of the list. chibicc writes one .byte per character. */
static void print_data(void)
{
    for (const struct obj *var = globals; var != NULL; var = var->next) {
        if (var->is_function) {
            continue;
        }
        P("  .local %s\n  .data\n%s:\n  .string \"", var->name, var->name);
        for (const char *c = var->init_data; *c != '\0'; c++) {
            if (*c == '\n') {
                P("\\n");
            } else {
                P("%c", *c);
            }
        }
        P("\"\n");
    }
}

static void stage_codegen(void)
{
    emit_text();
    P("== stage_codegen: assembly ==\n");
    print_data();
    P("  .text\n");
    for (int i = 0; i < ncode; i++) {
        char text[96];
        fmt_insn(text, sizeof text, &code[i]);
        const int flush_left = (code[i].op == A_LABEL) | (code[i].op == A_GLOBL);
        P("%s%s\n", flush_left ? "" : "  ", text);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_encode
 *
 * chibicc writes text and leaves the encoding to `as`, and the linking to `ld`. This stage
 * does both in one pass: every instruction becomes bytes, the label of each is recorded, and
 * the rel32 fields of jumps and rip-relative addresses are patched at the end. The runtime
 * (print_int and print_str, which a real program takes from libc) and the strings follow the
 * code in the same buffer.
 * ---------------------------------------------------------------------------------------- */

#define CODE_SIZE 4096
static uint8_t out[CODE_SIZE];
static size_t out_len;

struct label_def {
    char name[SYM];
    size_t off;
};
struct fixup {
    size_t at;
    char sym[SYM];
};
static struct label_def labels[MAX_LABELS];
static int nlabels;
static struct fixup fixups[MAX_LABELS];
static int nfixups;

#define EMIT(...)                                                                                  \
    do {                                                                                           \
        const uint8_t bytes_[] = {__VA_ARGS__};                                                    \
        memcpy(&out[out_len], bytes_, sizeof bytes_);                                              \
        out_len += sizeof bytes_;                                                                  \
    } while (0)

static void emit32(uint32_t v)
{
    EMIT((uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24));
}

static uint8_t imm8(int64_t v)
{
    require((v >= -128) & (v <= 127), "an immediate that needs 32 bits");
    return (uint8_t)v;
}

static void add_label(const char *name)
{
    require(nlabels < MAX_LABELS, "more labels");
    snprintf(labels[nlabels].name, SYM, "%s", name);
    labels[nlabels++].off = out_len;
}

static size_t find_label(const char *name)
{
    int i = 0;
    while (strcmp(labels[i].name, name) != 0) {
        i++;
    }
    return labels[i].off;
}

static void emit_rel32(const char *sym)
{
    require(nfixups < MAX_LABELS, "more fixups");
    fixups[nfixups].at = out_len;
    snprintf(fixups[nfixups++].sym, SYM, "%s", sym);
    emit32(0);
}

static void emit_insn(const struct insn *in)
{
    const int r1 = in->r1;
    const int r2 = in->r2;
    switch (in->op) {
    case A_GLOBL:
        break;
    case A_LABEL:
        add_label(in->sym);
        break;
    case A_PUSH:
        require(r1 < 8, "a push of r8 or above");
        EMIT((uint8_t)(0x50 + r1));
        break;
    case A_POP:
        require(r1 < 8, "a pop into r8 or above");
        EMIT((uint8_t)(0x58 + r1));
        break;
    case A_MOV_RI:
        EMIT((uint8_t)(0x48 | (r1 >> 3)), 0xc7, (uint8_t)(0xc0 | (r1 & 7)));
        emit32((uint32_t)in->imm);
        break;
    case A_MOV_RR:
        EMIT((uint8_t)(0x48 | ((r1 >> 3) << 2) | (r2 >> 3)), 0x89,
             (uint8_t)(0xc0 | ((r1 & 7) << 3) | (r2 & 7)));
        break;
    case A_LEA_RBP:
        EMIT(0x48, 0x8d, (uint8_t)(0x45 | (r1 << 3)), imm8(in->imm));
        break;
    case A_LEA_RIP:
        EMIT(0x48, 0x8d, 0x05);
        emit_rel32(in->sym);
        break;
    case A_LOAD32S:
        EMIT(0x48, 0x63, 0x00);
        break;
    case A_STORE32:
        EMIT(0x89, 0x07);
        break;
    case A_STORE_RBP32:
        require(r1 < 8, "a parameter in r8 or above");
        EMIT(0x89, (uint8_t)(0x45 | (r1 << 3)), imm8(in->imm));
        break;
    case A_STORE_RBP64:
        EMIT(0x48, 0x89, (uint8_t)(0x45 | (r1 << 3)), imm8(in->imm));
        break;
    case A_ADD:
        EMIT(0x01, 0xf8);
        break;
    case A_SUB:
        EMIT(0x29, 0xf8);
        break;
    case A_IMUL:
        EMIT(0x0f, 0xaf, 0xc7);
        break;
    case A_CMP:
        EMIT(0x39, 0xf8);
        break;
    case A_CMP0:
        EMIT(0x83, 0xf8, 0x00);
        break;
    case A_SETL:
        EMIT(0x0f, 0x9c, 0xc0);
        break;
    case A_MOVZB:
        EMIT(0x48, 0x0f, 0xb6, 0xc0);
        break;
    case A_JE:
        EMIT(0x0f, 0x84);
        emit_rel32(in->sym);
        break;
    case A_JMP:
        EMIT(0xe9);
        emit_rel32(in->sym);
        break;
    case A_CALL_R10:
        EMIT(0x41, 0xff, 0xd2);
        break;
    case A_ADD_RSP:
        EMIT(0x48, 0x83, 0xc4, imm8(in->imm));
        break;
    case A_SUB_RSP:
        EMIT(0x48, 0x83, 0xec, imm8(in->imm));
        break;
    case A_MOV_AL0:
        EMIT(0xb0, 0x00);
        break;
    case A_REP_STOSB:
        EMIT(0xf3, 0xaa);
        break;
    default: /* A_RET */
        EMIT(0xc3);
        break;
    }
}

static void stage_encode(void)
{
    P("== stage_encode: machine code, with the offset of each instruction ==\n");
    for (int i = 0; i < ncode; i++) {
        char text[96];
        fmt_insn(text, sizeof text, &code[i]);
        P("  +0x%04zx  %s\n", out_len, text);
        emit_insn(&code[i]);
    }

    /* print_int(rdi): mov rax, rdi; then the digits are built backwards on the stack. */
    add_label("print_int");
    EMIT(0x48, 0x89, 0xf8, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x74, 0x24, 0x20,
         0x48, 0xc7, 0xc1, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xff, 0xce, 0xc6, 0x06, 0x0a,
         0x31, 0xd2, 0x48, 0xf7, 0xf1, 0x80, 0xc2, 0x30, 0x48, 0xff, 0xce, 0x88, 0x16,
         0x48, 0x85, 0xc0, 0x75, 0xee,
         0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0x29, 0xf2, 0xb8, 0x01, 0x00, 0x00, 0x00,
         0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x20, 0xc3);
    /* print_str(rdi): rdi points at a NUL-terminated string, whose length is found first. */
    add_label("print_str");
    EMIT(0x48, 0x89, 0xfe, 0x31, 0xd2, 0x80, 0x3c, 0x17, 0x00, 0x74, 0x05, 0x48, 0xff, 0xc2,
         0xeb, 0xf5, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05,
         0xc3);
    for (const struct obj *var = globals; var != NULL; var = var->next) {
        if (var->is_function) {
            continue;
        }
        add_label(var->name);
        memcpy(&out[out_len], var->init_data, (size_t)var->ty->size);
        out_len += (size_t)var->ty->size;
    }

    for (int i = 0; i < nfixups; i++) {
        const size_t target = find_label(fixups[i].sym);
        const uint32_t rel = (uint32_t)(int32_t)((int64_t)target - (int64_t)(fixups[i].at + 4));
        for (int j = 0; j < 4; j++) {
            out[fixups[i].at + (size_t)j] = (uint8_t)(rel >> (8 * j));
        }
        P("+0x%04zx  rel32 %d to %s at +0x%04zx\n", fixups[i].at, (int32_t)rel, fixups[i].sym,
          target);
    }
    P("== stage_encode: final code, %zu bytes ==", out_len);
    for (size_t i = 0; i < out_len; i++) {
        if (i % 16 == 0) {
            P("\n+0x%04zx:", i);
        }
        P(" %02x", out[i]);
    }
    P("\n");
}

/* ------------------------------------------------------------------------------------------
 * stage_export
 * ---------------------------------------------------------------------------------------- */

static void stage_export(const char *path)
{
    P("== stage_export ==\n");
    if (path != NULL) {
        FILE *fp = fopen(path, "wb");
        // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
        if (fp == NULL || fwrite(out, 1, out_len, fp) != out_len || fclose(fp) != 0) {
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
    memcpy(buf, out, out_len);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (mprotect(buf, CODE_SIZE, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    /* ISO C forbids casting an object pointer to a function pointer, so copy the bits. */
    void *entry = (uint8_t *)buf + find_label("main");
    void (*fn)(void);
    memcpy(&fn, &entry, sizeof fn);
    fn();
}

int main(int argc, char **argv)
{
    const struct program *program = NULL;
    if (argc < 2 || (program = find_program(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    stage_ast(program);
    stage_assign_offsets();
    stage_codegen();
    stage_encode();
    stage_export(argc > 2 ? argv[2] : NULL);
    return 0;
}
