/*
 * sea-of-nodes: a cut-down version of the compiler in Cliff Click's SeaOfNodes/Simple.
 *
 * The program becomes one graph in which control flow and data flow are both edges between
 * nodes. Data nodes do not belong to a basic block while they are optimized: they float, and a
 * later scheduling pass (global code motion) picks their block. The same graph is then
 * lowered in place to machine nodes, scheduled, colored with registers and encoded.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout, the graph after each stage on stderr, and writes
 *   the machine code to code.bin if a path is given.
 *
 * Stages, in the order of the phases of Simple's CodeGen (chapter 21):
 *   stage_parse        build the graph; nodes are folded and shared as they are created
 *   stage_iter         peepholes that collapse dead control flow, then drop unreachable nodes
 *   stage_loop_tree    find the loops and the dominator tree
 *   stage_inst_select  rewrite ideal nodes into x86-64 machine nodes, in place
 *   stage_schedule     global code motion: put every floating node in a block
 *   stage_local_sched  order the nodes inside each block
 *   stage_reg_alloc    graph coloring, with splitting when a live range cannot be colored
 *   stage_encode       emit bytes
 *   stage_export       link the functions in memory and run main
 */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------------------------------
 * Nodes
 *
 * Input 0 of a control node is the control it follows. Input 0 of a data node is empty while
 * it floats, and global code motion fills it in with the node's block. A Phi has its Region
 * in input 0. A Region has input 0 empty and one control input per predecessor, from 1 on.
 * ---------------------------------------------------------------------------------------- */

enum kind {
    K_START,
    K_XCTRL, /* the control of dead code */
    K_FUN,
    K_PARM,
    K_CONST,
    K_STR,
    K_ADD,
    K_SUB,
    K_MUL,
    K_LT,
    K_IF,
    K_CPROJ, /* con is 0 for the true side and 1 for the false side */
    K_REGION,
    K_LOOP,
    K_PHI,
    K_CALL,
    K_CALLEND,
    K_RESULT,
    K_RETURN,
    K_STOP,
    /* The machine nodes that instruction selection rewrites ideal nodes into. */
    K_MOVI,
    K_LEA,
    K_ADDM,
    K_ADDI,
    K_SUBI,
    K_MULM,
    K_CMPI,
    K_JCC,
    K_CALLM,
    K_SPLIT,
};

static const char *const kind_names[] = {
    "Start", "XCtrl", "Fun", "Parm", "Con", "Str", "Add", "Sub", "Mul", "LT", "If", "CProj",
    "Region", "Loop", "Phi", "Call", "CallEnd", "Result", "Return", "Stop",
    "movi", "lea", "add", "addi", "subi", "imul", "cmpi", "jl", "call", "split",
};

#define MAX_IN 4
#define MAX_NODES 512

struct node {
    int id;
    enum kind kind;
    struct node *in[MAX_IN];
    int n_in;
    int64_t con; /* constant, CProj side, call target, parameter index or string index */
    int fun;     /* the function the node was created in */
    struct node *block; /* set by scheduling: the block head that holds this node */
    /* Only block heads have these. */
    struct node *idom;
    int idepth;
    int loop_depth;
    int block_index;
    int lrg;     /* set by register allocation */
    int reg;
};

static struct node nodes[MAX_NODES];
static int node_count;
static int reachable[MAX_NODES];

static struct node *new_node(enum kind kind, int fun, int n_in, struct node *a, struct node *b,
                             struct node *c)
{
    struct node *n = &nodes[node_count];
    memset(n, 0, sizeof *n);
    n->id = node_count++;
    n->kind = kind;
    n->fun = fun;
    n->n_in = n_in;
    n->in[0] = a;
    n->in[1] = b;
    n->in[2] = c;
    return n;
}

#define HELLO_STRING "Hello, world!\n"

/* ------------------------------------------------------------------------------------------
 * Graph construction with peepholes
 *
 * A node is folded when its inputs are constants, and shared when an equal node exists
 * (global value numbering). Both happen as the node is created, so the graph is never larger
 * than it needs to be. Control flow gets the same treatment, but only in stage_iter.
 * ---------------------------------------------------------------------------------------- */

#define MAX_FUNS 2
static struct node *start, *stop;
static struct node *fun_head[MAX_FUNS];
static struct node *gvn_table[MAX_NODES];
static int gvn_count;

static struct node *con(int fun, int64_t value);

static int64_t fold(enum kind kind, int64_t a, int64_t b)
{
    /* Lua integers wrap around, so compute on unsigned values. */
    const uint64_t x = (uint64_t)a, y = (uint64_t)b;
    switch (kind) {
    case K_ADD:
        return (int64_t)(x + y);
    case K_SUB:
        return (int64_t)(x - y);
    case K_MUL:
        return (int64_t)(x * y);
    default:
        return a < b;
    }
}

/* Bitwise & instead of &&: this is one decision, not one per field. */
static int same_node(const struct node *a, const struct node *b)
{
    return (a->kind == b->kind) & (a->con == b->con) & (a->in[0] == b->in[0])
         & (a->in[1] == b->in[1]) & (a->in[2] == b->in[2]);
}

/* Called on the node that was just created, which is the last one in the pool. */
static struct node *peephole(struct node *n)
{
    if ((unsigned)(n->kind - K_ADD) <= (unsigned)(K_LT - K_ADD)) { /* Add, Sub, Mul or LT */
        if ((n->in[1]->kind == K_CONST) & (n->in[2]->kind == K_CONST)) {
            const int64_t value = fold(n->kind, n->in[1]->con, n->in[2]->con);
            const int fun = n->fun;
            node_count--;
            return con(fun, value);
        }
    }
    for (int i = 0; i < gvn_count; i++) {
        if (same_node(gvn_table[i], n)) {
            node_count--;
            return gvn_table[i];
        }
    }
    gvn_table[gvn_count++] = n;
    return n;
}

/* A constant hangs off its function's Fun node. Simple hangs it off Start and then copies it
 * for every function that uses it, so that is the same graph. */
static struct node *con(int fun, int64_t value)
{
    struct node *n = new_node(K_CONST, fun, 1, fun_head[fun], NULL, NULL);
    n->con = value;
    return peephole(n);
}

/* Is the control that this node represents unreachable? */
static int dead(const struct node *n)
{
    switch (n->kind) {
    case K_XCTRL:
        return 1;
    case K_FUN:
        return 0;
    case K_LOOP: /* only the entry decides, the back edge cannot make it live */
        return dead(n->in[1]);
    case K_CPROJ: {
        const struct node *test = n->in[0];
        const int constant_test = test->in[1]->kind == K_CONST;
        return dead(test) | (constant_test & ((test->in[1]->con != 0) != (n->con == 0)));
    }
    case K_REGION: {
        int live = 0;
        for (int i = 1; i < n->n_in; i++) {
            live |= !dead(n->in[i]);
        }
        return !live;
    }
    default: /* If, Call and CallEnd are as dead as the control they follow */
        return dead(n->in[0]);
    }
}

/* ------------------------------------------------------------------------------------------
 * The parser's state. Simple parses a function body into the graph; here the example is
 * hardcoded and drives the same operations: define a variable, read it, branch, loop, call.
 * ---------------------------------------------------------------------------------------- */

#define MAX_VARS 4
#define NATIVE_PRINT_INT (-1)
#define NATIVE_PRINT_STR (-2)

struct scope {
    const char *name[MAX_VARS];
    struct node *value[MAX_VARS];
    int count;
};

struct fn_state {
    struct node *fun;
    struct node *ctrl;
    struct scope scope;
    struct node *ret_region; /* every return adds an input to this Region and its Phi */
    struct node *ret_phi;
    struct node *if_false;   /* the pending false side of the innermost if */
    struct node *if_true_end;
    struct scope loop_exit;  /* the scope after the loop test */
    struct node *loop_head;
};

static struct fn_state fstack[2];
static int fdepth = -1;
static int fun_count;
static const char *fun_names[MAX_FUNS];
#define F (fstack[fdepth])

static struct node *emit(enum kind kind, int n_in, struct node *a, struct node *b, struct node *c)
{
    return peephole(new_node(kind, F.fun->fun, n_in, a, b, c));
}

static struct node *num(int64_t value) { return con(F.fun->fun, value); }
static struct node *add(struct node *a, struct node *b) { return emit(K_ADD, 3, NULL, a, b); }
static struct node *sub(struct node *a, struct node *b) { return emit(K_SUB, 3, NULL, a, b); }
static struct node *mul(struct node *a, struct node *b) { return emit(K_MUL, 3, NULL, a, b); }
static struct node *lt(struct node *a, struct node *b) { return emit(K_LT, 3, NULL, a, b); }

static struct node *str(int index)
{
    struct node *n = new_node(K_STR, F.fun->fun, 1, F.fun, NULL, NULL);
    n->con = index;
    return peephole(n);
}

static void define(const char *name, struct node *value)
{
    F.scope.name[F.scope.count] = name;
    F.scope.value[F.scope.count++] = value;
}

static struct node *get(const char *name)
{
    int i = 0;
    while (strcmp(F.scope.name[i], name) != 0) {
        i++;
    }
    return F.scope.value[i];
}

static void set(const char *name, struct node *value)
{
    int i = 0;
    while (strcmp(F.scope.name[i], name) != 0) {
        i++;
    }
    F.scope.value[i] = value;
}

/* Start a function. Its Return is built once every return statement has added its input. */
static void fn_begin(const char *name, int has_param)
{
    fdepth++;
    const int index = fun_count++;
    fun_names[index] = name;
    memset(&F, 0, sizeof F);
    F.fun = new_node(K_FUN, index, 1, start, NULL, NULL);
    F.fun->con = index;
    fun_head[index] = F.fun;
    F.ctrl = F.fun;
    F.ret_region = new_node(K_REGION, index, 1, NULL, NULL, NULL);
    F.ret_phi = new_node(K_PHI, index, 1, F.ret_region, NULL, NULL);
    if (has_param) {
        struct node *parm = new_node(K_PARM, index, 1, F.fun, NULL, NULL);
        define("n", parm);
    }
}

static struct node *call(int64_t target, struct node *arg)
{
    struct node *c = new_node(K_CALL, F.fun->fun, 2, F.ctrl, arg, NULL);
    c->con = target;
    struct node *end = new_node(K_CALLEND, F.fun->fun, 1, c, NULL, NULL);
    F.ctrl = end;
    return new_node(K_RESULT, F.fun->fun, 1, end, NULL, NULL);
}

static void if_begin(struct node *pred)
{
    struct node *test = new_node(K_IF, F.fun->fun, 2, F.ctrl, pred, NULL);
    struct node *t = new_node(K_CPROJ, F.fun->fun, 1, test, NULL, NULL);
    struct node *f = new_node(K_CPROJ, F.fun->fun, 1, test, NULL, NULL);
    f->con = 1;
    F.if_false = f;
    F.ctrl = t;
}

/* Join the end of the true side with the false side. There is no else part in any example,
 * so the false side is just the pending false projection. */
static void if_end(void)
{
    struct node *r = new_node(K_REGION, F.fun->fun, 3, NULL, F.ctrl, F.if_false);
    F.ctrl = r;
}

static void ret_stmt(struct node *value)
{
    F.ret_region->in[F.ret_region->n_in] = F.ctrl;
    F.ret_phi->in[F.ret_phi->n_in] = value;
    F.ret_region->n_in++;
    F.ret_phi->n_in++;
    F.ctrl = peephole(new_node(K_XCTRL, 0, 1, NULL, NULL, NULL));
}

/* The loop head is a Region whose back edge is not known yet, so every variable gets a Phi
 * with a blank back-edge input. Simple makes these Phis eagerly too. */
static void while_begin(void)
{
    struct node *loop = new_node(K_LOOP, F.fun->fun, 3, NULL, F.ctrl, NULL);
    F.loop_head = loop;
    F.ctrl = loop;
    for (int i = 0; i < F.scope.count; i++) {
        struct node *phi = new_node(K_PHI, F.fun->fun, 3, loop, F.scope.value[i], NULL);
        F.scope.value[i] = phi;
    }
}

static void while_cond(struct node *pred)
{
    F.loop_exit = F.scope; /* the test does not change variables, so this is the scope after it */
    if_begin(pred);
}

static void while_end(void)
{
    F.loop_head->in[2] = F.ctrl;
    for (int i = 0; i < F.scope.count; i++) {
        F.loop_exit.value[i]->in[2] = F.scope.value[i]; /* the Phi at the loop head */
    }
    F.scope = F.loop_exit;
    F.ctrl = F.if_false;
}

static void fn_end(struct node *final_value)
{
    if (F.ctrl->kind != K_XCTRL) {
        ret_stmt(final_value);
    }
    struct node *ret = new_node(K_RETURN, F.fun->fun, 2, F.ret_region, F.ret_phi, NULL);
    stop->in[stop->n_in++] = ret;
    fdepth--;
}

/* ------------------------------------------------------------------------------------------
 * Printing the graph
 * ---------------------------------------------------------------------------------------- */

static void print_node(const struct node *n)
{
    fprintf(stderr, "  N%-3d %-8s", n->id, kind_names[n->kind]);
    for (int i = 0; i < n->n_in; i++) {
        if (n->in[i] == NULL) {
            fprintf(stderr, " ___");
        } else {
            fprintf(stderr, " N%d", n->in[i]->id);
        }
    }
    switch (n->kind) {
    case K_STR:
    case K_CONST:
    case K_MOVI:
    case K_ADDI:
    case K_SUBI:
    case K_CMPI:
        fprintf(stderr, "   #%ld", (long)n->con);
        break;
    case K_CPROJ:
        fprintf(stderr, "   %s", n->con == 0 ? "true" : "false");
        break;
    case K_FUN:
        fprintf(stderr, "   %s", fun_names[n->con]);
        break;
    case K_CALL:
    case K_CALLM:
        fprintf(stderr, "   %s",
                n->con == NATIVE_PRINT_INT   ? "print_int"
                : n->con == NATIVE_PRINT_STR ? "print_str"
                                             : fun_names[n->con]);
        break;
    default:
        break;
    }
    fprintf(stderr, "\n");
}

static void print_graph(const char *title, int only_reachable)
{
    fprintf(stderr, "== %s ==\n", title);
    for (int i = 0; i < node_count; i++) {
        if (!only_reachable || reachable[i]) {
            print_node(&nodes[i]);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * The examples, hardcoded as the calls the parser would make
 * ---------------------------------------------------------------------------------------- */

/* print("Hello, world!") */
static void parse_hello(void)
{
    fn_begin("main", 0);
    call(NATIVE_PRINT_STR, str(0));
    fn_end(num(0));
}

/* print((2 + 3) * 4 - 6) */
static void parse_arithmetic(void)
{
    fn_begin("main", 0);
    /* C does not say which argument is evaluated first, and the node numbers follow the order,
     * so the operands are built left to right in separate statements, as the parser would. */
    struct node *two = num(2);
    struct node *three = num(3);
    struct node *sum = add(two, three);
    struct node *product = mul(sum, num(4));
    call(NATIVE_PRINT_INT, sub(product, num(6)));
    fn_end(num(0));
}

/*
 * local a = 3
 * local b = 5
 * if a < b then
 *   print(b)
 * else
 *   print(a)
 * end
 */
static void parse_branch(void)
{
    fn_begin("main", 0);
    define("a", num(3));
    define("b", num(5));
    if_begin(lt(get("a"), get("b")));
    call(NATIVE_PRINT_INT, get("b"));
    struct node *true_end = F.ctrl;
    F.ctrl = F.if_false;
    call(NATIVE_PRINT_INT, get("a"));
    F.if_false = F.ctrl;
    F.ctrl = true_end;
    if_end();
    fn_end(num(0));
}

/*
 * local sum = 0
 * local i = 1
 * while i < 11 do
 *   sum = sum + i
 *   i = i + 1
 * end
 * print(sum)
 */
static void parse_loop(void)
{
    fn_begin("main", 0);
    define("sum", num(0));
    define("i", num(1));
    while_begin();
    while_cond(lt(get("i"), num(11)));
    set("sum", add(get("sum"), get("i")));
    set("i", add(get("i"), num(1)));
    while_end();
    call(NATIVE_PRINT_INT, get("sum"));
    fn_end(num(0));
}

/*
 * local function fact(n)
 *   if n < 2 then
 *     return 1
 *   end
 *   return n * fact(n - 1)
 * end
 * print(fact(5))
 */
static void parse_call(void)
{
    fn_begin("main", 0);

    fn_begin("fact", 1);
    if_begin(lt(get("n"), num(2)));
    ret_stmt(num(1));
    if_end();
    struct node *rest = call(1, sub(get("n"), num(1)));
    ret_stmt(mul(get("n"), rest));
    fn_end(NULL);

    struct node *result = call(1, num(5));
    call(NATIVE_PRINT_INT, result);
    fn_end(num(0));
}

struct program {
    const char *name;
    void (*parse)(void);
};

static const struct program programs[] = {
    {"hello", parse_hello}, {"arithmetic", parse_arithmetic}, {"branch", parse_branch},
    {"loop", parse_loop},   {"call", parse_call},
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

static void stage_parse(const struct program *program)
{
    start = new_node(K_START, 0, 1, NULL, NULL, NULL);
    stop = new_node(K_STOP, 0, 1, NULL, NULL, NULL);
    program->parse();
    print_graph("stage_parse: graph as built, constants folded and shared", 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_iter
 * ---------------------------------------------------------------------------------------- */

static void replace(struct node *old, struct node *with)
{
    for (int i = 0; i < node_count; i++) {
        for (int j = 0; j < nodes[i].n_in; j++) {
            if (nodes[i].in[j] == old) {
                nodes[i].in[j] = with;
            }
        }
    }
}

static void mark(const struct node *n)
{
    if (n == NULL || reachable[n->id]) {
        return;
    }
    reachable[n->id] = 1;
    for (int i = 0; i < n->n_in; i++) {
        mark(n->in[i]);
    }
}

/* A Region with one live input is that input, and its Phis are the matching value. */
static int collapse_region(struct node *region)
{
    int live_count = 0, live_index = 0;
    for (int i = 1; i < region->n_in; i++) {
        if (!dead(region->in[i])) {
            live_count++;
            live_index = i;
        }
    }
    if (live_count != 1) {
        return 0;
    }
    for (int i = 0; i < node_count; i++) {
        if (nodes[i].kind == K_PHI && nodes[i].in[0] == region) {
            replace(&nodes[i], nodes[i].in[live_index]);
            nodes[i].kind = K_XCTRL; /* unused now, left out of the output by mark */
        }
    }
    replace(region, region->in[live_index]);
    fprintf(stderr, "N%d Region has one live input, replaced by N%d\n", region->id,
            region->in[live_index]->id);
    return 1;
}

/* The live side of an If with a constant test is just the control the If followed. */
static int fold_constant_if(struct node *proj)
{
    struct node *test = proj->in[0];
    if (test->in[1]->kind != K_CONST || dead(proj)) {
        return 0;
    }
    fprintf(stderr, "N%d CProj follows a constant If, replaced by N%d\n", proj->id,
            test->in[0]->id);
    replace(proj, test->in[0]);
    return 1;
}

static void stage_iter(void)
{
    fprintf(stderr, "== stage_iter: peepholes until nothing changes ==\n");
    mark(stop);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < node_count; i++) {
            if (!reachable[i]) {
                continue;
            }
            if (nodes[i].kind == K_REGION) {
                changed |= collapse_region(&nodes[i]);
            } else if (nodes[i].kind == K_CPROJ) {
                changed |= fold_constant_if(&nodes[i]);
            }
        }
        if (changed) {
            memset(reachable, 0, sizeof reachable);
            mark(stop);
        }
    }
    print_graph("stage_iter: reachable graph", 1);
}

/* ------------------------------------------------------------------------------------------
 * The control flow graph
 *
 * A basic block is named by its head: the Fun, Region, Loop, CProj or CallEnd node. The nodes
 * that follow it, up to an If, a Call or a Return, belong to the block, and so does every
 * data node that scheduling puts there.
 * ---------------------------------------------------------------------------------------- */

static int is_head(const struct node *n)
{
    return n->kind == K_FUN || n->kind == K_REGION || n->kind == K_LOOP || n->kind == K_CPROJ
        || n->kind == K_CALLEND;
}

static struct node *block_of(struct node *n)
{
    while (!is_head(n)) {
        n = n->in[0];
    }
    return n;
}

static int pred_count(const struct node *head)
{
    if (head->kind == K_FUN) {
        return 0;
    }
    if (head->kind == K_REGION || head->kind == K_LOOP) {
        return head->n_in - 1;
    }
    return 1;
}

static struct node *pred_block(const struct node *head, int i)
{
    if (head->kind == K_REGION || head->kind == K_LOOP) {
        return block_of(head->in[i + 1]);
    }
    return block_of(head->in[0]);
}

/* ------------------------------------------------------------------------------------------
 * stage_loop_tree
 *
 * A loop is the set of blocks that reach the back edge of a Loop head without passing through
 * the head. Every block gets the number of loops it is in.
 * ---------------------------------------------------------------------------------------- */

static int in_loop[MAX_NODES];

static void loop_body(struct node *block)
{
    if (in_loop[block->id]) {
        return;
    }
    in_loop[block->id] = 1;
    block->loop_depth++;
    for (int i = 0; i < pred_count(block); i++) {
        loop_body(pred_block(block, i));
    }
}

/* The blocks of every function in reverse postorder, one function after the other. */
#define MAX_BLOCKS 24
static struct node *rpo[MAX_BLOCKS];
static int block_count;

static int successors(const struct node *b, struct node **out)
{
    int count = 0;
    for (int i = 0; i < node_count; i++) {
        if (!reachable[i] || !is_head(&nodes[i])) {
            continue;
        }
        for (int j = 0; j < pred_count(&nodes[i]); j++) {
            if (pred_block(&nodes[i], j) == b) {
                out[count++] = &nodes[i];
            }
        }
    }
    return count;
}

static int visited[MAX_NODES];
static struct node *postorder[MAX_BLOCKS];
static int post_count;

static void dfs(struct node *b)
{
    visited[b->id] = 1;
    struct node *next[MAX_BLOCKS];
    const int n = successors(b, next);
    for (int i = 0; i < n; i++) {
        if (!visited[next[i]->id]) {
            dfs(next[i]);
        }
    }
    postorder[post_count++] = b;
}

static struct node *lca(struct node *a, struct node *b)
{
    while (a != b) {
        if (a->idepth >= b->idepth) {
            a = a->idom;
        } else {
            b = b->idom;
        }
    }
    return a;
}

static void stage_loop_tree(void)
{
    /* The loop depth of a block is the number of loops it is in. */
    for (int i = 0; i < node_count; i++) {
        if (reachable[i] && nodes[i].kind == K_LOOP) {
            memset(in_loop, 0, sizeof in_loop);
            in_loop[i] = 1;
            nodes[i].loop_depth++;
            loop_body(pred_block(&nodes[i], 1));
        }
    }
    for (int f = 0; f < fun_count; f++) {
        const int first = post_count;
        dfs(fun_head[f]);
        for (int i = post_count - 1; i >= first; i--) {
            rpo[block_count] = postorder[i];
            postorder[i]->block_index = block_count++;
        }
    }
    /* A Region is dominated by the nearest block that dominates all its inputs. A Loop only by
     * its entry, since the back edge is not known to be reachable without passing the Loop. */
    for (int i = 0; i < block_count; i++) {
        struct node *h = rpo[i];
        if (h->kind == K_FUN) {
            continue;
        }
        struct node *d = pred_block(h, 0);
        for (int j = 1; j < pred_count(h) && h->kind == K_REGION; j++) {
            d = lca(d, pred_block(h, j));
        }
        h->idom = d;
        h->idepth = d->idepth + 1;
    }
    fprintf(stderr, "== stage_loop_tree: blocks in reverse postorder ==\n");
    for (int i = 0; i < block_count; i++) {
        const struct node *h = rpo[i];
        fprintf(stderr, "  block %d: N%-3d %-8s idom N%-3d idepth %d  loop depth %d\n", i, h->id,
                kind_names[h->kind], h->idom == NULL ? -1 : h->idom->id, h->idepth,
                h->loop_depth);
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_inst_select
 *
 * Simple matches patterns against the graph (x86_64_v2.java). Each ideal node here has one
 * pattern, and the node is rewritten in place, so everything that used it follows along.
 * Only the shapes that the examples contain are matched: an Add or Sub with a constant right
 * operand becomes an immediate form, and a comparison with a constant feeds a branch.
 * ---------------------------------------------------------------------------------------- */

static void to_immediate(struct node *n, enum kind kind)
{
    n->con = n->in[2]->con;
    n->in[2] = NULL;
    n->n_in = 2;
    n->kind = kind;
}

static void stage_inst_select(void)
{
    for (int i = 0; i < node_count; i++) {
        struct node *n = &nodes[i];
        if (!reachable[i]) {
            continue;
        }
        switch (n->kind) {
        case K_ADD:
            if (n->in[2]->kind == K_CONST) {
                to_immediate(n, K_ADDI);
            } else {
                n->kind = K_ADDM;
            }
            break;
        case K_SUB: /* the examples only subtract a constant */
            to_immediate(n, K_SUBI);
            break;
        case K_MUL:
            n->kind = K_MULM;
            break;
        case K_LT: /* the examples only compare with a constant */
            to_immediate(n, K_CMPI);
            break;
        case K_IF:
            n->kind = K_JCC;
            break;
        case K_CALL:
            n->kind = K_CALLM;
            break;
        default:
            break;
        }
    }
    /* Constants are rewritten last, so the patterns above still see them as constants. */
    for (int i = 0; i < node_count; i++) {
        if (reachable[i] && nodes[i].kind == K_CONST) {
            nodes[i].kind = K_MOVI;
        } else if (reachable[i] && nodes[i].kind == K_STR) {
            nodes[i].kind = K_LEA;
        }
    }
    /* A constant that became an immediate has no users left. */
    memset(reachable, 0, sizeof reachable);
    mark(stop);
    print_graph("stage_inst_select: machine nodes", 1);
}

/* ------------------------------------------------------------------------------------------
 * stage_schedule
 *
 * Global code motion. Control nodes, Phis, Parms and Results are pinned to a block. Every
 * other node floats, and goes to the nearest block that dominates all its uses, which sinks it
 * out of paths that do not need it. A use by a Phi counts in the matching predecessor.
 * Simple also schedules early and hoists out of loops; no example has a value to hoist.
 * The dominator tree is therefore only used to find the loop depths and the Region inputs.
 * ---------------------------------------------------------------------------------------- */

static int is_floating(const struct node *n)
{
    return n->kind == K_MOVI || n->kind == K_LEA || n->kind == K_ADDM || n->kind == K_ADDI
        || n->kind == K_SUBI || n->kind == K_MULM;
}

/* The block of the use of a floating node. After break_up_constants every floating node of the
 * examples has exactly one use. Simple takes the lowest common dominator of all of them. */
static struct node *schedule_late(struct node *n)
{
    if (!is_floating(n)) {
        return n->block;
    }
    struct node *best = NULL;
    for (int u = 0; u < node_count; u++) {
        for (int k = 1; reachable[u] && k < nodes[u].n_in; k++) {
            if (nodes[u].in[k] == n) {
                best = nodes[u].kind == K_PHI ? pred_block(nodes[u].in[0], k - 1)
                                              : schedule_late(&nodes[u]);
            }
        }
    }
    return best;
}

/* Simple's breakUpGlobalConstants: a constant that many nodes share would be one long live
 * range, so every use after the first gets its own copy, which is then scheduled on its own. */
static void break_up_constants(void)
{
    for (int i = 0; i < node_count; i++) {
        struct node *c = &nodes[i];
        if (!reachable[i] || (c->kind != K_MOVI && c->kind != K_LEA)) {
            continue;
        }
        int seen = 0;
        for (int u = 0; u < node_count; u++) {
            for (int k = 1; reachable[u] && k < nodes[u].n_in; k++) {
                if (nodes[u].in[k] != c || seen++ == 0) {
                    continue;
                }
                struct node *copy = new_node(c->kind, c->fun, 1, NULL, NULL, NULL);
                copy->con = c->con;
                reachable[copy->id] = 1;
                nodes[u].in[k] = copy;
                fprintf(stderr, "  N%d is a copy of N%d for N%d\n", copy->id, c->id, nodes[u].id);
            }
        }
    }
}

static void stage_schedule(void)
{
    fprintf(stderr, "== stage_schedule: constants shared by several uses get one copy per use ==\n");
    break_up_constants();
    for (int i = 0; i < node_count; i++) {
        struct node *n = &nodes[i];
        if (reachable[i] && (n->kind == K_PHI || n->kind == K_PARM || n->kind == K_RESULT
                             || n->kind == K_JCC || n->kind == K_CALLM || n->kind == K_RETURN)) {
            n->block = block_of(n);
        }
    }
    for (int i = 0; i < node_count; i++) {
        /* The compare stays with its branch, because the branch reads the flags. */
        if (reachable[i] && nodes[i].kind == K_JCC) {
            nodes[i].in[1]->block = nodes[i].block;
        }
    }
    fprintf(stderr, "== stage_schedule: block of each floating node ==\n");
    for (int i = 0; i < node_count; i++) {
        struct node *n = &nodes[i];
        if (reachable[i] && is_floating(n)) {
            n->block = schedule_late(n);
            n->in[0] = n->block;
            fprintf(stderr, "  N%-3d %-5s goes to block N%d\n", n->id, kind_names[n->kind],
                    n->block->id);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_local_sched
 *
 * Order the nodes inside each block: Phis, then the Parm and Result, then the floating nodes
 * with inputs first, then the compare, then the branch, call or return.
 * ---------------------------------------------------------------------------------------- */

#define MAX_BLOCK_NODES 40
static struct node *block_code[MAX_BLOCKS][MAX_BLOCK_NODES];
static int code_count[MAX_BLOCKS];
static int phi_count[MAX_BLOCKS];
static int placed[MAX_NODES];

static void append(const struct node *b, struct node *n)
{
    block_code[b->block_index][code_count[b->block_index]++] = n;
    placed[n->id] = 1;
}

static void place(const struct node *b, struct node *n)
{
    if (placed[n->id]) {
        return;
    }
    for (int i = 1; i < n->n_in; i++) {
        if (n->in[i]->block == b) {
            place(b, n->in[i]);
        }
    }
    append(b, n);
}

static void place_kind(const struct node *b, enum kind kind)
{
    for (int i = 0; i < node_count; i++) {
        if (reachable[i] && nodes[i].block == b && nodes[i].kind == kind) {
            append(b, &nodes[i]);
        }
    }
}

static void print_instr(const struct node *n, int with_regs);

static void print_blocks(const char *title, int with_regs)
{
    fprintf(stderr, "== %s ==\n", title);
    for (int i = 0; i < block_count; i++) {
        fprintf(stderr, "  block N%d (%s)\n", rpo[i]->id, kind_names[rpo[i]->kind]);
        for (int j = 0; j < code_count[i]; j++) {
            print_instr(block_code[i][j], with_regs);
            fprintf(stderr, "\n");
        }
    }
}

static void stage_local_sched(void)
{
    for (int bi = 0; bi < block_count; bi++) {
        const struct node *b = rpo[bi];
        place_kind(b, K_PHI);
        phi_count[bi] = code_count[bi];
        place_kind(b, K_PARM);
        place_kind(b, K_RESULT);
        for (int i = 0; i < node_count; i++) {
            if (reachable[i] && nodes[i].block == b && is_floating(&nodes[i])) {
                place(b, &nodes[i]);
            }
        }
        place_kind(b, K_CMPI);
        place_kind(b, K_JCC);
        place_kind(b, K_CALLM);
        place_kind(b, K_RETURN);
    }
    print_blocks("stage_local_sched: nodes in each block, in order", 0);
}

/* ------------------------------------------------------------------------------------------
 * stage_reg_alloc
 *
 * Chaitin-style graph coloring over live ranges (LRGs), with the structure of Simple's
 * RegAlloc: every round builds the LRGs, builds the interference graph and tries to color it.
 * When that fails, it adds copies (SPLIT nodes) at the place that failed and starts over.
 *
 *  1. An LRG is a set of nodes that must share one register: a Phi and its inputs, and a
 *     two-address instruction (add, imul, sub) and its first input. It carries a mask of the
 *     registers it may use, the intersection of what each member's def and uses accept.
 *  2. Two LRGs interfere when one is defined while the other is live. A def into a single
 *     register does not add edges, it takes that register away from everything live there.
 *     A call takes every caller-save register from the LRGs that live across it.
 *  3. An LRG that interferes with itself needs a copy: before a two-address instruction.
 *  4. An LRG with an empty mask gets a copy after each def into a single register and before
 *     each use that needs a single register.
 *  5. Otherwise the graph is colored by repeatedly removing an LRG with fewer neighbors than
 *     registers in its mask, then giving colors back in the opposite order.
 * ---------------------------------------------------------------------------------------- */

enum { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSI = 6, RDI = 7, R8 = 8, R9 = 9, R10 = 10, R11 = 11 };
#define M(r) (1u << (r))
#define ALL_REGS (M(RAX) | M(RCX) | M(RDX) | M(RBX) | M(RSI) | M(RDI) | M(R8) | M(R9) | M(R10) | M(R11))
#define CALLER_SAVE (ALL_REGS & ~M(RBX))

static const char *const reg_names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                          "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

static int is_value(const struct node *n)
{
    return n->kind == K_PARM || n->kind == K_RESULT || n->kind == K_PHI || n->kind == K_MOVI
        || n->kind == K_LEA || n->kind == K_ADDM || n->kind == K_ADDI || n->kind == K_SUBI
        || n->kind == K_MULM || n->kind == K_SPLIT;
}

/* The first input of these is also where the result goes. */
static int is_tied(const struct node *n)
{
    return n->kind == K_ADDM || n->kind == K_ADDI || n->kind == K_SUBI || n->kind == K_MULM;
}

static unsigned def_mask(const struct node *n)
{
    return n->kind == K_PARM ? M(RDI) : n->kind == K_RESULT ? M(RAX) : ALL_REGS;
}

static unsigned use_mask(const struct node *n)
{
    return n->kind == K_CALLM ? M(RDI) : n->kind == K_RETURN ? M(RAX) : ALL_REGS;
}

static int parent[MAX_NODES];
static unsigned mask[MAX_NODES];
static unsigned base_mask[MAX_NODES];
static unsigned char adj[MAX_NODES][MAX_NODES];
static unsigned char live_in[MAX_BLOCKS][MAX_NODES];
static unsigned char live_out[MAX_BLOCKS][MAX_NODES];
static int color[MAX_NODES];
static struct node *conflict;

static int find(int x)
{
    while (parent[x] != x) {
        x = parent[x];
    }
    return x;
}

static void join(int a, int b)
{
    parent[find(a)] = find(b);
}

#define EACH_INSTR(bi, k, n)                                                                       \
    for (int bi = 0; bi < block_count; bi++)                                                       \
        for (int k = 0; k < code_count[bi]; k++)                                                   \
            for (struct node *n = block_code[bi][k]; n != NULL; n = NULL)

static void build_lrgs(void)
{
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n)) {
            parent[n->id] = n->id;
            mask[n->id] = ALL_REGS;
        }
    }
    EACH_INSTR(bi, k, n)
    {
        for (int i = 1; i < n->n_in && n->kind == K_PHI; i++) {
            join(n->id, n->in[i]->id);
        }
        if (is_tied(n)) {
            join(n->id, n->in[1]->id);
        }
    }
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n)) {
            mask[find(n->id)] &= def_mask(n);
        }
        for (int i = 1; i < n->n_in; i++) {
            if (is_value(n->in[i])) {
                mask[find(n->in[i]->id)] &= use_mask(n);
            }
        }
    }
    memcpy(base_mask, mask, sizeof mask);
}

static void liveness(void)
{
    memset(live_in, 0, sizeof live_in);
    memset(live_out, 0, sizeof live_out);
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int bi = block_count - 1; bi >= 0; bi--) {
            struct node *succ[MAX_BLOCKS];
            const int count = successors(rpo[bi], succ);
            for (int s = 0; s < count; s++) {
                const int si = succ[s]->block_index;
                for (int v = 0; v < node_count; v++) {
                    live_out[bi][v] |= live_in[si][v];
                }
                /* A Phi input is used at the end of the matching predecessor. */
                for (int i = 0; i < pred_count(succ[s]); i++) {
                    if (pred_block(succ[s], i) != rpo[bi]) {
                        continue;
                    }
                    for (int k = 0; k < phi_count[si]; k++) {
                        live_out[bi][block_code[si][k]->in[i + 1]->id] = 1;
                    }
                }
            }
            unsigned char cur[MAX_NODES];
            memcpy(cur, live_out[bi], sizeof cur);
            for (int k = code_count[bi] - 1; k >= 0; k--) {
                const struct node *n = block_code[bi][k];
                if (is_value(n)) {
                    cur[n->id] = 0;
                }
                for (int i = 1; i < n->n_in && n->kind != K_PHI; i++) {
                    if (is_value(n->in[i])) {
                        cur[n->in[i]->id] = 1;
                    }
                }
            }
            if (memcmp(cur, live_in[bi], sizeof cur) != 0) {
                memcpy(live_in[bi], cur, sizeof cur);
                changed = 1;
            }
        }
    }
}

/* Node d is defined while everything in live is live. */
static void def_point(struct node *d, const unsigned char *live)
{
    const int rd = find(d->id);
    for (int v = 0; v < node_count; v++) {
        if (!live[v] || v == d->id) {
            continue;
        }
        const int rv = find(v);
        if (rv == rd) {
            conflict = d;
        } else if (__builtin_popcount(base_mask[rd]) == 1) {
            mask[rv] &= ~base_mask[rd];
        } else {
            adj[rd][rv] = adj[rv][rd] = 1;
        }
    }
}

static void build_ifg(void)
{
    memset(adj, 0, sizeof adj);
    conflict = NULL;
    for (int bi = 0; bi < block_count; bi++) {
        unsigned char live[MAX_NODES];
        memcpy(live, live_out[bi], sizeof live);
        const int phis = phi_count[bi];
        for (int k = code_count[bi] - 1; k >= phis; k--) {
            struct node *n = block_code[bi][k];
            for (int v = 0; n->kind == K_CALLM && v < node_count; v++) {
                if (live[v]) {
                    mask[find(v)] &= ~CALLER_SAVE;
                }
            }
            if (is_value(n)) {
                def_point(n, live);
                live[n->id] = 0;
            }
            for (int i = 1; i < n->n_in; i++) {
                if (is_value(n->in[i])) {
                    live[n->in[i]->id] = 1;
                }
            }
        }
        /* The Phis of a block are defined together, so they all see the same live set. */
        for (int k = 0; k < phis; k++) {
            def_point(block_code[bi][k], live);
        }
    }
}

/* Put a copy of v in the list of block b at position pos, and return it. */
static struct node *insert_split(struct node *b, int pos, struct node *v)
{
    struct node *s = new_node(K_SPLIT, v->fun, 2, b, v, NULL);
    s->block = b;
    struct node **list = block_code[b->block_index];
    memmove(&list[pos + 1], &list[pos], (size_t)(code_count[b->block_index] - pos) * sizeof *list);
    list[pos] = s;
    code_count[b->block_index]++;
    return s;
}

static int position(const struct node *n)
{
    int k = 0;
    while (block_code[n->block->block_index][k] != n) {
        k++;
    }
    return k;
}

static void split_before(struct node *user, int operand)
{
    fprintf(stderr, "  split N%d before N%d\n", user->in[operand]->id, user->id);
    user->in[operand] = insert_split(user->block, position(user), user->in[operand]);
}

static void split_after(struct node *def)
{
    struct node *s = insert_split(def->block, position(def) + 1, def);
    fprintf(stderr, "  split N%d after its def as N%d\n", def->id, s->id);
    EACH_INSTR(bi, k, n)
    {
        for (int i = 1; i < n->n_in; i++) {
            if (n->in[i] == def && n != s) {
                n->in[i] = s;
            }
        }
    }
}

/* Splits around the single-register defs and uses of the LRG whose root is r. */
static void split_empty(int r)
{
    struct node *defs[8];
    struct node *users[8];
    int operands[8];
    int def_count = 0, use_count = 0;
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n) && find(n->id) == r && __builtin_popcount(def_mask(n)) == 1) {
            defs[def_count++] = n;
        }
        for (int i = 1; i < n->n_in; i++) {
            if (is_value(n->in[i]) && find(n->in[i]->id) == r
                && __builtin_popcount(use_mask(n)) == 1) {
                users[use_count] = n;
                operands[use_count++] = i;
            }
        }
    }
    for (int i = 0; i < def_count; i++) {
        split_after(defs[i]);
    }
    for (int i = 0; i < use_count; i++) {
        split_before(users[i], operands[i]);
    }
}

static void print_regs(const char *title);

/* Returns 1 when it had to split, in which case the caller must start another round. */
static int fix_failure(void)
{
    if (conflict != NULL) {
        fprintf(stderr, "  N%d needs a register different from its first input, which stays live\n",
                conflict->id);
        split_before(conflict, 1);
        return 1;
    }
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n) && find(n->id) == n->id && mask[n->id] == 0) {
            fprintf(stderr, "  the LRG of N%d has no register left\n", n->id);
            split_empty(n->id);
            return 1;
        }
    }
    return 0;
}

static void color_graph(void)
{
    int roots[MAX_NODES], root_count = 0;
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n) && find(n->id) == n->id) {
            roots[root_count++] = n->id;
        }
    }
    int removed[MAX_NODES] = {0};
    int stack[MAX_NODES], top = 0;
    for (;;) {
        int pick = -1;
        for (int i = 0; i < root_count && pick < 0; i++) {
            const int r = roots[i];
            int degree = 0;
            for (int j = 0; j < root_count; j++) {
                degree += adj[r][roots[j]] && !removed[roots[j]];
            }
            if (!removed[r] && degree < __builtin_popcount(mask[r])) {
                pick = r;
            }
        }
        if (pick < 0) {
            break;
        }
        removed[pick] = 1;
        stack[top++] = pick;
    }
    // GCOVR_EXCL_START: every graph of the five examples is colored by this simple removal
    if (top != root_count) {
        fprintf(stderr, "coloring failed, this toy has no optimistic coloring or spilling\n");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    for (int i = 0; i < root_count; i++) {
        color[roots[i]] = -1;
    }
    while (top > 0) {
        const int r = stack[--top];
        unsigned avail = mask[r];
        for (int j = 0; j < root_count; j++) {
            if (adj[r][roots[j]] && color[roots[j]] >= 0) {
                avail &= ~M(color[roots[j]]);
            }
        }
        color[r] = __builtin_ctz(avail);
    }
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n)) {
            n->lrg = find(n->id);
            n->reg = color[n->lrg];
        }
    }
}

static void stage_reg_alloc(void)
{
    fprintf(stderr, "== stage_reg_alloc: rounds ==\n");
    int round = 1;
    for (;; round++) {
        // GCOVR_EXCL_START: a shape that the examples do not have would split without end
        if (round > 20) {
            fprintf(stderr, "no progress after 20 rounds of splitting\n");
            exit(1);
        }
        // GCOVR_EXCL_STOP
        fprintf(stderr, "round %d\n", round);
        build_lrgs();
        liveness();
        build_ifg();
        if (!fix_failure()) {
            break;
        }
    }
    color_graph();
    print_regs("stage_reg_alloc: registers");
}

static void print_instr(const struct node *n, int with_regs)
{
    fprintf(stderr, "    N%-3d ", n->id);
    if (with_regs) {
        if (is_value(n)) {
            fprintf(stderr, "%-4s = ", reg_names[n->reg]);
        } else {
            fprintf(stderr, "       ");
        }
    }
    fprintf(stderr, n->kind == K_PARM || n->kind == K_RESULT ? "%s" : "%-6s", kind_names[n->kind]);
    for (int i = 1; i < n->n_in; i++) {
        fprintf(stderr, " N%d", n->in[i]->id);
        if (with_regs && is_value(n->in[i])) {
            fprintf(stderr, "(%s)", reg_names[n->in[i]->reg]);
        }
    }
    switch (n->kind) {
    case K_MOVI:
    case K_ADDI:
    case K_SUBI:
    case K_CMPI:
        fprintf(stderr, " #%ld", (long)n->con);
        break;
    case K_LEA:
        fprintf(stderr, " string %ld", (long)n->con);
        break;
    case K_CALLM:
        fprintf(stderr, " %s",
                n->con == NATIVE_PRINT_INT   ? "print_int"
                : n->con == NATIVE_PRINT_STR ? "print_str"
                                             : fun_names[n->con]);
        break;
    default:
        break;
    }
}

static void print_regs(const char *title)
{
    fprintf(stderr, "LRGs\n");
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n) && n->lrg == n->id) {
            fprintf(stderr, "  LRG N%-3d %-4s members:", n->id, reg_names[n->reg]);
            EACH_INSTR(bj, kk, m)
            {
                if (is_value(m) && m->lrg == n->id) {
                    fprintf(stderr, " N%d", m->id);
                }
            }
            fprintf(stderr, "\n");
        }
    }
    print_blocks(title, 1);
}

/* ------------------------------------------------------------------------------------------
 * stage_encode
 *
 * Every machine node becomes x86-64 bytes. A call, a jump and a string address are rel32
 * fields whose target is only known later, so they are recorded and patched at the end. The
 * runtime (print_int and print_str, which are native functions in Simple) and the strings
 * follow the functions in the same buffer.
 * ---------------------------------------------------------------------------------------- */

#define CODE_SIZE 4096
static uint8_t out[CODE_SIZE];
static size_t out_len;

enum { FIX_BLOCK, FIX_FUN, FIX_PRINT_INT, FIX_PRINT_STR, FIX_STRING };
struct fixup {
    size_t at;
    int kind;
    int target;
};
static struct fixup fixups[32];
static int fixup_count;

static size_t block_off[MAX_BLOCKS];
static size_t fun_off[MAX_FUNS];
static size_t print_int_off, print_str_off, string_off;

static const char *const strings[] = {HELLO_STRING};

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

static void emit_rel32(int kind, int target)
{
    fixups[fixup_count++] = (struct fixup){out_len, kind, target};
    emit32(0);
}

/* op reg, rm with both operands registers: REX.W, opcode, ModRM. */
static void emit_rr(uint8_t op, int reg, int rm)
{
    EMIT(0x48 | ((reg >> 3) << 2) | (rm >> 3), op, 0xc0 | ((reg & 7) << 3) | (rm & 7));
}

/* op rm, imm32 where ext is the opcode extension in the reg field. */
static void emit_ri(uint8_t op, int ext, int rm, int64_t imm)
{
    EMIT(0x48 | (rm >> 3), op, 0xc0 | (ext << 3) | (rm & 7));
    emit32((uint32_t)imm);
}

static void emit_node(const struct node *n)
{
    const int dst = n->reg;
    switch (n->kind) {
    case K_MOVI:
        emit_ri(0xc7, 0, dst, n->con);
        break;
    case K_LEA:
        EMIT(0x48 | ((dst >> 3) << 2), 0x8d, ((dst & 7) << 3) | 5);
        emit_rel32(FIX_STRING, (int)n->con);
        break;
    case K_ADDM:
        emit_rr(0x01, n->in[2]->reg, dst);
        break;
    case K_MULM:
        EMIT(0x48 | ((dst >> 3) << 2) | (n->in[2]->reg >> 3), 0x0f, 0xaf,
             0xc0 | ((dst & 7) << 3) | (n->in[2]->reg & 7));
        break;
    case K_ADDI:
        emit_ri(0x81, 0, dst, n->con);
        break;
    case K_SUBI:
        emit_ri(0x81, 5, dst, n->con);
        break;
    case K_CMPI:
        emit_ri(0x81, 7, n->in[1]->reg, n->con);
        break;
    case K_SPLIT:
        if (dst != n->in[1]->reg) {
            emit_rr(0x89, n->in[1]->reg, dst);
        }
        break;
    default: /* K_CALLM, the only other node that emits code and has no terminator part */
        EMIT(0xe8);
        emit_rel32(n->con == NATIVE_PRINT_INT   ? FIX_PRINT_INT
                   : n->con == NATIVE_PRINT_STR ? FIX_PRINT_STR
                                                : FIX_FUN,
                   (int)n->con);
        break;
    }
}

static void emit_jump(const struct node *to, const struct node *next)
{
    if (to != next) {
        EMIT(0xe9);
        emit_rel32(FIX_BLOCK, to->block_index);
    }
}

static int uses_rbx[MAX_FUNS];

static void emit_block(int bi)
{
    const struct node *b = rpo[bi];
    const struct node *next = bi + 1 < block_count && rpo[bi + 1]->kind != K_FUN ? rpo[bi + 1] : NULL;
    if (b->kind == K_FUN) {
        fun_off[b->fun] = out_len;
        if (uses_rbx[b->fun]) {
            EMIT(0x53);
        }
    }
    block_off[bi] = out_len;
    struct node *succ[2];
    const int succ_count = successors(b, succ);
    for (int k = 0; k < code_count[bi]; k++) {
        const struct node *n = block_code[bi][k];
        fprintf(stderr, "  +0x%04zx", out_len);
        print_instr(n, 1);
        fprintf(stderr, "\n");
        if (n->kind == K_RETURN) {
            if (uses_rbx[b->fun]) {
                EMIT(0x5b);
            }
            EMIT(0xc3);
        } else if (n->kind == K_JCC) {
            /* The CProj with con 0 is the true side. */
            const struct node *yes = succ[succ[0]->con];
            const struct node *no = succ[1 - succ[0]->con];
            EMIT(0x0f, 0x8c); /* jl */
            emit_rel32(FIX_BLOCK, yes->block_index);
            emit_jump(no, next);
        } else if (n->kind != K_PHI && n->kind != K_PARM && n->kind != K_RESULT) {
            emit_node(n);
        }
    }
    /* A block with one successor jumps to it, unless that block follows. */
    if (succ_count == 1) {
        emit_jump(succ[0], next);
    }
}

static void stage_encode(void)
{
    fprintf(stderr, "== stage_encode: machine code, with the offset of each node ==\n");
    EACH_INSTR(bi, k, n)
    {
        if (is_value(n) && n->reg == RBX) {
            uses_rbx[rpo[bi]->fun] = 1;
        }
    }
    for (int bi = 0; bi < block_count; bi++) {
        emit_block(bi);
    }

    /* print_int(rdi): mov rax, rdi; then the digits are built backwards on the stack. */
    print_int_off = out_len;
    EMIT(0x48, 0x89, 0xf8, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x74, 0x24, 0x20,
         0x48, 0xc7, 0xc1, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xff, 0xce, 0xc6, 0x06, 0x0a,
         0x31, 0xd2, 0x48, 0xf7, 0xf1, 0x80, 0xc2, 0x30, 0x48, 0xff, 0xce, 0x88, 0x16,
         0x48, 0x85, 0xc0, 0x75, 0xee,
         0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0x29, 0xf2, 0xb8, 0x01, 0x00, 0x00, 0x00,
         0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x20, 0xc3);
    /* print_str(rdi): rdi points at a length byte followed by the characters. */
    print_str_off = out_len;
    EMIT(0x0f, 0xb6, 0x17, 0x48, 0x8d, 0x77, 0x01, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x01,
         0x00, 0x00, 0x00, 0x0f, 0x05, 0xc3);
    string_off = out_len;
    out[out_len++] = (uint8_t)strlen(strings[0]);
    memcpy(&out[out_len], strings[0], strlen(strings[0]));
    out_len += strlen(strings[0]);

    for (int i = 0; i < fixup_count; i++) {
        const struct fixup f = fixups[i];
        const size_t target = f.kind == FIX_BLOCK       ? block_off[f.target]
                              : f.kind == FIX_FUN       ? fun_off[f.target]
                              : f.kind == FIX_PRINT_INT ? print_int_off
                              : f.kind == FIX_PRINT_STR ? print_str_off
                                                        : string_off;
        const uint32_t rel = (uint32_t)(int32_t)((int64_t)target - (int64_t)(f.at + 4));
        for (int j = 0; j < 4; j++) {
            out[f.at + (size_t)j] = (uint8_t)(rel >> (8 * j));
        }
        fprintf(stderr, "+0x%04zx  rel32 %d to +0x%04zx\n", f.at, (int32_t)rel, target);
    }
    fprintf(stderr, "== stage_encode: final code, %zu bytes ==", out_len);
    for (size_t i = 0; i < out_len; i++) {
        if (i % 16 == 0) {
            fprintf(stderr, "\n+0x%04zx:", i);
        }
        fprintf(stderr, " %02x", out[i]);
    }
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------------------------------
 * stage_export
 * ---------------------------------------------------------------------------------------- */

static void stage_export(const char *path)
{
    fprintf(stderr, "== stage_export ==\n");
    if (path != NULL) {
        FILE *f = fopen(path, "wb");
        // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
        if (f == NULL || fwrite(out, 1, out_len, f) != out_len || fclose(f) != 0) {
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
    void *entry = (uint8_t *)buf + fun_off[0];
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
    stage_parse(program);
    stage_iter();
    stage_loop_tree();
    stage_inst_select();
    stage_schedule();
    stage_local_sched();
    stage_reg_alloc();
    stage_encode();
    stage_export(argc > 2 ? argv[2] : NULL);
    return 0;
}
