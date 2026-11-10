/*
 * jlox: a cut-down version of the tree-walking interpreter from "Crafting Interpreters".
 *
 * jlox parses the source into a syntax tree and runs the tree directly, with no bytecode.
 * Between the two sits a resolver: a pass over the finished tree that works out, for every
 * variable use, how many scopes up its declaration is. The interpreter keeps those distances
 * in a side table keyed by tree node, and at run time it walks that many links up a chain of
 * environments instead of searching by name. A name the resolver did not find is a global.
 *
 * This entry skips the scanner and the parser: the syntax tree of each example is built by
 * hand with small constructor functions. The resolver, the environments, closures and the
 * tree walker are the real design in small.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. An interpreter generates no machine code, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source   build the syntax tree of the example and print it (stands in for the
 *                  scanner and the parser) in the parenthesized style of AstPrinter
 *   stage_resolve  walk the tree with a stack of scopes and record each use's distance
 *   stage_run      walk the tree, printing every variable access and call
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------
 * Memory: everything is freed in one go at exit, because there is no garbage collector
 * ---------------------------------------------------------------------------------------- */

union alloc_header {
    union alloc_header *next;
    max_align_t align;
};

static union alloc_header *all_allocations;

static void *allocate(size_t size)
{
    union alloc_header *header = calloc(1, sizeof(*header) + size);
    if (header == NULL) { /* GCOVR_EXCL_LINE: allocation failure is not tested */
        abort(); /* GCOVR_EXCL_LINE: allocation failure is not tested */
    }
    header->next = all_allocations;
    all_allocations = header;
    return header + 1;
}

static void free_all(void)
{
    while (all_allocations != NULL) {
        union alloc_header *next = all_allocations->next;
        free(all_allocations);
        all_allocations = next;
    }
}

/* ------------------------------------------------------------------------------------------
 * The syntax tree (Expr.java and Stmt.java)
 * ---------------------------------------------------------------------------------------- */

enum expr_kind {
    EXPR_NUMBER,
    EXPR_STRING,
    EXPR_VARIABLE,
    EXPR_ASSIGN,
    EXPR_BINARY,
    EXPR_CALL,
};

struct expr {
    enum expr_kind kind;
    double number; /* EXPR_NUMBER */
    const char *text; /* EXPR_STRING: the characters, EXPR_VARIABLE and EXPR_ASSIGN: the name */
    char op; /* EXPR_BINARY */
    struct expr *left; /* EXPR_BINARY: left operand, EXPR_ASSIGN: value, EXPR_CALL: callee */
    struct expr *right; /* EXPR_BINARY: right operand, EXPR_CALL: the one argument */
};

enum stmt_kind {
    STMT_PRINT,
    STMT_VAR,
    STMT_EXPRESSION,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_FUNCTION,
    STMT_RETURN,
};

struct stmt {
    enum stmt_kind kind;
    struct stmt *next; /* the next statement of the same list */
    const char *name; /* STMT_VAR and STMT_FUNCTION */
    const char *param; /* STMT_FUNCTION */
    struct expr *expr; /* the expression, the initializer, or the condition */
    struct stmt *body; /* a list for STMT_BLOCK and STMT_FUNCTION, one statement for STMT_IF and STMT_WHILE */
    struct stmt *else_branch; /* STMT_IF */
};

static struct expr *new_expr(enum expr_kind kind)
{
    struct expr *expr = allocate(sizeof(*expr));
    expr->kind = kind;
    return expr;
}

static struct expr *number(double value)
{
    struct expr *expr = new_expr(EXPR_NUMBER);
    expr->number = value;
    return expr;
}

static struct expr *string(const char *text)
{
    struct expr *expr = new_expr(EXPR_STRING);
    expr->text = text;
    return expr;
}

static struct expr *variable(const char *name)
{
    struct expr *expr = new_expr(EXPR_VARIABLE);
    expr->text = name;
    return expr;
}

static struct expr *assign(const char *name, struct expr *value)
{
    struct expr *expr = new_expr(EXPR_ASSIGN);
    expr->text = name;
    expr->left = value;
    return expr;
}

static struct expr *binary(struct expr *left, char op, struct expr *right)
{
    struct expr *expr = new_expr(EXPR_BINARY);
    expr->left = left;
    expr->op = op;
    expr->right = right;
    return expr;
}

static struct expr *call(struct expr *callee, struct expr *argument)
{
    struct expr *expr = new_expr(EXPR_CALL);
    expr->left = callee;
    expr->right = argument;
    return expr;
}

static struct stmt *new_stmt(enum stmt_kind kind, struct expr *expr)
{
    struct stmt *stmt = allocate(sizeof(*stmt));
    stmt->kind = kind;
    stmt->expr = expr;
    return stmt;
}

/* Link the statements into a list. The argument list ends with NULL. */
static struct stmt *seq(struct stmt *first, ...)
{
    va_list args;
    va_start(args, first);
    struct stmt *last = first;
    for (struct stmt *stmt = va_arg(args, struct stmt *); stmt != NULL;
         stmt = va_arg(args, struct stmt *)) {
        last->next = stmt;
        last = stmt;
    }
    va_end(args);
    return first;
}

static struct stmt *print_stmt(struct expr *expr)
{
    return new_stmt(STMT_PRINT, expr);
}

static struct stmt *var_stmt(const char *name, struct expr *initializer)
{
    struct stmt *stmt = new_stmt(STMT_VAR, initializer);
    stmt->name = name;
    return stmt;
}

static struct stmt *expression_stmt(struct expr *expr)
{
    return new_stmt(STMT_EXPRESSION, expr);
}

static struct stmt *block(struct stmt *body)
{
    struct stmt *stmt = new_stmt(STMT_BLOCK, NULL);
    stmt->body = body;
    return stmt;
}

static struct stmt *if_stmt(struct expr *condition, struct stmt *then_branch,
                            struct stmt *else_branch)
{
    struct stmt *stmt = new_stmt(STMT_IF, condition);
    stmt->body = then_branch;
    stmt->else_branch = else_branch;
    return stmt;
}

static struct stmt *while_stmt(struct expr *condition, struct stmt *body)
{
    struct stmt *stmt = new_stmt(STMT_WHILE, condition);
    stmt->body = body;
    return stmt;
}

static struct stmt *function(const char *name, const char *param, struct stmt *body)
{
    struct stmt *stmt = new_stmt(STMT_FUNCTION, NULL);
    stmt->name = name;
    stmt->param = param;
    stmt->body = body;
    return stmt;
}

static struct stmt *return_stmt(struct expr *value)
{
    return new_stmt(STMT_RETURN, value);
}

/* ------------------------------------------------------------------------------------------
 * The examples, as the tree the parser would build
 * ---------------------------------------------------------------------------------------- */

static struct stmt *program_hello(void)
{
    /* print "Hello, world!"; */
    return print_stmt(string("Hello, world!"));
}

static struct stmt *program_arithmetic(void)
{
    /* print (2 + 3) * 4 - 6; */
    return print_stmt(
        binary(binary(binary(number(2), '+', number(3)), '*', number(4)), '-', number(6)));
}

static struct stmt *program_branch(void)
{
    /* { var a = 3; var b = 5; if (a < b) { print b; } else { print a; } } */
    return block(seq(var_stmt("a", number(3)), var_stmt("b", number(5)),
                     if_stmt(binary(variable("a"), '<', variable("b")),
                             block(print_stmt(variable("b"))),
                             block(print_stmt(variable("a")))),
                     NULL));
}

static struct stmt *program_loop(void)
{
    /* { var sum = 0; var i = 1;
     *   while (i < 11) { sum = sum + i; i = i + 1; }
     *   print sum; } */
    return block(seq(
        var_stmt("sum", number(0)), var_stmt("i", number(1)),
        while_stmt(binary(variable("i"), '<', number(11)),
                   block(seq(expression_stmt(
                                 assign("sum", binary(variable("sum"), '+', variable("i")))),
                             expression_stmt(
                                 assign("i", binary(variable("i"), '+', number(1)))),
                             NULL))),
        print_stmt(variable("sum")), NULL));
}

static struct stmt *program_call(void)
{
    /* fun fact(n) { if (n < 2) { return 1; } return n * fact(n - 1); }
     * print fact(5); */
    return seq(function("fact", "n",
                        seq(if_stmt(binary(variable("n"), '<', number(2)),
                                    block(return_stmt(number(1))), NULL),
                            return_stmt(binary(variable("n"), '*',
                                               call(variable("fact"),
                                                    binary(variable("n"), '-', number(1))))),
                            NULL)),
               print_stmt(call(variable("fact"), number(5))), NULL);
}

static struct stmt *build_program(const char *name)
{
    if (strcmp(name, "hello") == 0) {
        return program_hello();
    }
    if (strcmp(name, "arithmetic") == 0) {
        return program_arithmetic();
    }
    if (strcmp(name, "branch") == 0) {
        return program_branch();
    }
    if (strcmp(name, "loop") == 0) {
        return program_loop();
    }
    if (strcmp(name, "call") == 0) {
        return program_call();
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------
 * Printing the tree (in the style of AstPrinter.java, but one statement per line)
 * ---------------------------------------------------------------------------------------- */

static void print_expr(const struct expr *expr)
{
    switch (expr->kind) {
    case EXPR_NUMBER:
        fprintf(stderr, "%g", expr->number);
        break;
    case EXPR_STRING:
        fprintf(stderr, "\"%s\"", expr->text);
        break;
    case EXPR_VARIABLE:
        fprintf(stderr, "%s", expr->text);
        break;
    case EXPR_ASSIGN:
        fprintf(stderr, "(= %s ", expr->text);
        print_expr(expr->left);
        fprintf(stderr, ")");
        break;
    case EXPR_BINARY:
        fprintf(stderr, "(%c ", expr->op);
        print_expr(expr->left);
        fprintf(stderr, " ");
        print_expr(expr->right);
        fprintf(stderr, ")");
        break;
    default: /* EXPR_CALL */
        fprintf(stderr, "(call ");
        print_expr(expr->left);
        fprintf(stderr, " ");
        print_expr(expr->right);
        fprintf(stderr, ")");
        break;
    }
}

static void print_stmts(const struct stmt *stmt, int indent);

static void print_stmt_node(const struct stmt *stmt, int indent)
{
    fprintf(stderr, "%*s", indent * 2, "");
    switch (stmt->kind) {
    case STMT_PRINT:
        fprintf(stderr, "(print ");
        print_expr(stmt->expr);
        fprintf(stderr, ")\n");
        break;
    case STMT_VAR:
        fprintf(stderr, "(var %s ", stmt->name);
        print_expr(stmt->expr);
        fprintf(stderr, ")\n");
        break;
    case STMT_EXPRESSION:
        fprintf(stderr, "(; ");
        print_expr(stmt->expr);
        fprintf(stderr, ")\n");
        break;
    case STMT_BLOCK:
        fprintf(stderr, "(block\n");
        print_stmts(stmt->body, indent + 1);
        fprintf(stderr, "%*s)\n", indent * 2, "");
        break;
    case STMT_IF:
        fprintf(stderr, "(if ");
        print_expr(stmt->expr);
        fprintf(stderr, "\n");
        print_stmt_node(stmt->body, indent + 1);
        if (stmt->else_branch != NULL) {
            print_stmt_node(stmt->else_branch, indent + 1);
        }
        fprintf(stderr, "%*s)\n", indent * 2, "");
        break;
    case STMT_WHILE:
        fprintf(stderr, "(while ");
        print_expr(stmt->expr);
        fprintf(stderr, "\n");
        print_stmt_node(stmt->body, indent + 1);
        fprintf(stderr, "%*s)\n", indent * 2, "");
        break;
    case STMT_FUNCTION:
        fprintf(stderr, "(fun %s (%s)\n", stmt->name, stmt->param);
        print_stmts(stmt->body, indent + 1);
        fprintf(stderr, "%*s)\n", indent * 2, "");
        break;
    default: /* STMT_RETURN */
        fprintf(stderr, "(return ");
        print_expr(stmt->expr);
        fprintf(stderr, ")\n");
        break;
    }
}

static void print_stmts(const struct stmt *stmt, int indent)
{
    for (; stmt != NULL; stmt = stmt->next) {
        print_stmt_node(stmt, indent);
    }
}

/* ------------------------------------------------------------------------------------------
 * The interpreter state: environments and the side table of resolved distances
 * ---------------------------------------------------------------------------------------- */

enum value_kind {
    VALUE_BOOL,
    VALUE_NUMBER,
    VALUE_STRING,
    VALUE_FUNCTION,
};

struct environment;

struct function_object {
    const struct stmt *declaration;
    struct environment *closure; /* the environment that was current when `fun` ran */
};

struct value {
    enum value_kind kind;
    int boolean;
    double number;
    const char *string;
    const struct function_object *function;
};

#define ENV_CAPACITY 4

/* One scope at run time: a few name/value pairs and a link to the scope around it. */
struct environment {
    struct environment *enclosing;
    int count;
    const char *names[ENV_CAPACITY];
    struct value values[ENV_CAPACITY];
};

/* Interpreter.locals: the distance of every expression the resolver found a declaration for. */
#define MAX_LOCALS 32

struct local_entry {
    const struct expr *expr;
    int depth;
};

static struct {
    struct environment globals;
    struct environment *environment;
    struct local_entry locals[MAX_LOCALS];
    int local_count;
    jmp_buf *catch_return; /* where `return` jumps to: the Return exception of jlox */
    struct value return_value;
    int call_depth; /* only for the trace */
} interp;

static struct environment *new_environment(struct environment *enclosing)
{
    struct environment *environment = allocate(sizeof(*environment));
    environment->enclosing = enclosing;
    return environment;
}

static void define(struct environment *environment, const char *name, struct value value)
{
    environment->names[environment->count] = name;
    environment->values[environment->count] = value;
    environment->count++;
}

static struct value *find(struct environment *environment, const char *name)
{
    /* The name is always there: the program only names declared variables. */
    for (int i = 0;; i++) {
        if (strcmp(environment->names[i], name) == 0) {
            return &environment->values[i];
        }
    }
}

/* Environment.getAt and assignAt: go up `distance` links, then look the name up there. */
static struct value *slot_at(struct environment *environment, int distance, const char *name)
{
    for (int i = 0; i < distance; i++) {
        environment = environment->enclosing;
    }
    return find(environment, name);
}

/* ------------------------------------------------------------------------------------------
 * The resolver (Resolver.java)
 * ---------------------------------------------------------------------------------------- */

#define MAX_SCOPES 8
#define SCOPE_CAPACITY 4

/* One compile-time scope: just the names declared in it. */
struct scope {
    const char *names[SCOPE_CAPACITY];
    int count;
};

static struct {
    struct scope scopes[MAX_SCOPES];
    int scope_count;
} resolver;

static void begin_scope(void)
{
    resolver.scopes[resolver.scope_count].count = 0;
    resolver.scope_count++;
}

static void end_scope(void)
{
    resolver.scope_count--;
}

/* Declaring and defining are one step here, see FIDELITY.md. At the top level there is no scope,
 * and the name becomes a global. */
static void declare(const char *name)
{
    if (resolver.scope_count == 0) {
        return;
    }
    struct scope *scope = &resolver.scopes[resolver.scope_count - 1];
    scope->names[scope->count] = name;
    scope->count++;
}

static void resolve_local(const struct expr *expr, const char *name)
{
    for (int i = resolver.scope_count - 1; i >= 0; i--) {
        const struct scope *scope = &resolver.scopes[i];
        for (int j = 0; j < scope->count; j++) {
            if (strcmp(scope->names[j], name) == 0) {
                int depth = resolver.scope_count - 1 - i;
                interp.locals[interp.local_count].expr = expr;
                interp.locals[interp.local_count].depth = depth;
                interp.local_count++;
                fprintf(stderr, "resolve %s: %d scope%s up\n", name, depth, depth == 1 ? "" : "s");
                return;
            }
        }
    }
    fprintf(stderr, "resolve %s: not found, so a global\n", name);
}

static void resolve_expr(const struct expr *expr)
{
    switch (expr->kind) {
    case EXPR_NUMBER:
    case EXPR_STRING:
        break;
    case EXPR_VARIABLE:
        resolve_local(expr, expr->text);
        break;
    case EXPR_ASSIGN:
        resolve_expr(expr->left);
        resolve_local(expr, expr->text);
        break;
    case EXPR_BINARY:
        resolve_expr(expr->left);
        resolve_expr(expr->right);
        break;
    default: /* EXPR_CALL */
        resolve_expr(expr->left);
        resolve_expr(expr->right);
        break;
    }
}

static void resolve_stmts(const struct stmt *stmt);

static void resolve_stmt(const struct stmt *stmt)
{
    switch (stmt->kind) {
    case STMT_PRINT:
    case STMT_EXPRESSION:
    case STMT_RETURN:
        resolve_expr(stmt->expr);
        break;
    case STMT_VAR:
        /* The initializer is resolved before the name is declared, so `var a = a;` would
         * look outwards. */
        resolve_expr(stmt->expr);
        declare(stmt->name);
        break;
    case STMT_BLOCK:
        fprintf(stderr, "begin scope %d\n", resolver.scope_count);
        begin_scope();
        resolve_stmts(stmt->body);
        end_scope();
        fprintf(stderr, "end scope %d\n", resolver.scope_count);
        break;
    case STMT_IF:
        resolve_expr(stmt->expr);
        resolve_stmt(stmt->body);
        if (stmt->else_branch != NULL) {
            resolve_stmt(stmt->else_branch);
        }
        break;
    case STMT_WHILE:
        resolve_expr(stmt->expr);
        resolve_stmt(stmt->body);
        break;
    default: /* STMT_FUNCTION */
        /* The function's name is declared first, so its body can call it. */
        declare(stmt->name);
        fprintf(stderr, "begin scope %d (function %s)\n", resolver.scope_count, stmt->name);
        begin_scope();
        declare(stmt->param);
        resolve_stmts(stmt->body);
        end_scope();
        fprintf(stderr, "end scope %d (function %s)\n", resolver.scope_count, stmt->name);
        break;
    }
}

static void resolve_stmts(const struct stmt *stmt)
{
    for (; stmt != NULL; stmt = stmt->next) {
        resolve_stmt(stmt);
    }
}

/* ------------------------------------------------------------------------------------------
 * The tree walker (Interpreter.java, LoxFunction.java)
 * ---------------------------------------------------------------------------------------- */

static struct value number_value(double n)
{
    struct value value = {.kind = VALUE_NUMBER, .number = n};
    return value;
}

static struct value bool_value(int boolean)
{
    struct value value = {.kind = VALUE_BOOL, .boolean = boolean};
    return value;
}

static struct value string_value(const char *text)
{
    struct value value = {.kind = VALUE_STRING, .string = text};
    return value;
}

static void trace(const char *format, ...)
{
    fprintf(stderr, "%*s", interp.call_depth * 2, "");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

/* The distance the resolver stored for this expression, or -1 when it is a global. */
static int distance_of(const struct expr *expr)
{
    for (int i = 0; i < interp.local_count; i++) {
        if (interp.locals[i].expr == expr) {
            return interp.locals[i].depth;
        }
    }
    return -1;
}

static struct value look_up_variable(const struct expr *expr)
{
    int distance = distance_of(expr);
    if (distance >= 0) {
        struct value value = *slot_at(interp.environment, distance, expr->text);
        trace("get %s from the scope %d up: %g", expr->text, distance, value.number);
        return value;
    }
    trace("get %s from the globals", expr->text);
    return *find(&interp.globals, expr->text);
}

static void execute_block(const struct stmt *body, struct environment *environment);
static void execute(const struct stmt *stmt);

static struct value evaluate(const struct expr *expr)
{
    switch (expr->kind) {
    case EXPR_NUMBER:
        return number_value(expr->number);
    case EXPR_STRING:
        return string_value(expr->text);
    case EXPR_VARIABLE:
        return look_up_variable(expr);
    case EXPR_ASSIGN: {
        struct value value = evaluate(expr->left);
        int distance = distance_of(expr);
        *slot_at(interp.environment, distance, expr->text) = value;
        trace("set %s in the scope %d up: %g", expr->text, distance, value.number);
        return value;
    }
    case EXPR_BINARY: {
        struct value left = evaluate(expr->left);
        struct value right = evaluate(expr->right);
        switch (expr->op) {
        case '+':
            return number_value(left.number + right.number);
        case '-':
            return number_value(left.number - right.number);
        case '*':
            return number_value(left.number * right.number);
        default:
            return bool_value(left.number < right.number);
        }
    }
    default: /* EXPR_CALL, handled below */
        break;
    }

    /* LoxFunction.call: a new environment whose parent is the closure, not the caller. */
    struct value callee = evaluate(expr->left);
    struct value argument = evaluate(expr->right);
    const struct stmt *declaration = callee.function->declaration;
    struct environment *environment = new_environment(callee.function->closure);
    define(environment, declaration->param, argument);
    trace("call %s(%g)", declaration->name, argument.number);

    struct environment *previous = interp.environment;
    jmp_buf *outer = interp.catch_return;
    jmp_buf here;
    interp.catch_return = &here;
    interp.call_depth++;
    if (setjmp(here) == 0) {
        execute_block(declaration->body, environment);
    }
    interp.call_depth--;
    interp.catch_return = outer;
    interp.environment = previous;
    trace("%s returned %g", declaration->name, interp.return_value.number);
    return interp.return_value;
}

static void execute_block(const struct stmt *body, struct environment *environment)
{
    struct environment *previous = interp.environment;
    interp.environment = environment;
    for (const struct stmt *stmt = body; stmt != NULL; stmt = stmt->next) {
        execute(stmt);
    }
    interp.environment = previous;
}

static void execute(const struct stmt *stmt)
{
    switch (stmt->kind) {
    case STMT_PRINT: {
        struct value value = evaluate(stmt->expr);
        if (value.kind == VALUE_STRING) {
            printf("%s\n", value.string);
        } else {
            printf("%g\n", value.number);
        }
        break;
    }
    case STMT_VAR: {
        struct value value = evaluate(stmt->expr);
        trace("define %s = %g", stmt->name, value.number);
        define(interp.environment, stmt->name, value);
        break;
    }
    case STMT_EXPRESSION:
        evaluate(stmt->expr);
        break;
    case STMT_BLOCK:
        trace("new environment for a block");
        execute_block(stmt->body, new_environment(interp.environment));
        break;
    case STMT_IF: {
        const struct stmt *taken = evaluate(stmt->expr).boolean ? stmt->body : stmt->else_branch;
        if (taken != NULL) {
            execute(taken);
        }
        break;
    }
    case STMT_WHILE:
        while (evaluate(stmt->expr).boolean) {
            execute(stmt->body);
        }
        break;
    case STMT_FUNCTION: {
        /* The closure is the environment that is current right now. */
        struct function_object *object = allocate(sizeof(*object));
        object->declaration = stmt;
        object->closure = interp.environment;
        struct value value = {.kind = VALUE_FUNCTION, .function = object};
        trace("define %s as a function closing over the current environment", stmt->name);
        define(interp.environment, stmt->name, value);
        break;
    }
    default: /* STMT_RETURN */
        interp.return_value = evaluate(stmt->expr);
        longjmp(*interp.catch_return, 1);
    }
}

/* ------------------------------------------------------------------------------------------
 * Stages
 * ---------------------------------------------------------------------------------------- */

static struct stmt *stage_source(const char *name)
{
    struct stmt *program = build_program(name);
    if (program != NULL) {
        fprintf(stderr, "== stage_source: syntax tree of %s ==\n", name);
        print_stmts(program, 0);
    }
    return program;
}

static void stage_resolve(const struct stmt *program)
{
    fprintf(stderr, "== stage_resolve: scope distance of every variable use ==\n");
    resolve_stmts(program);
}

static void stage_run(const struct stmt *program)
{
    fprintf(stderr, "== stage_run: every variable access and call ==\n");
    interp.environment = &interp.globals;
    for (const struct stmt *stmt = program; stmt != NULL; stmt = stmt->next) {
        execute(stmt);
    }
}

int main(int argc, char **argv)
{
    struct stmt *program = NULL;
    if (argc < 2 || (program = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }

    stage_resolve(program);
    stage_run(program);
    free_all();
    return 0;
}
