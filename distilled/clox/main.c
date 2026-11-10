/*
 * clox: a cut-down version of the bytecode interpreter from "Crafting Interpreters".
 *
 * clox has no syntax tree. A single-pass Pratt parser emits stack bytecode while it parses,
 * and a virtual machine runs the bytecode. Local variables are stack slots, and a closure
 * captures a local through an upvalue that points into the stack and is closed (copied into
 * the upvalue itself) when the local goes out of scope.
 *
 * This entry skips the scanner: the Lox tokens of each example are written down by hand. The
 * Pratt parser, the code generator and the virtual machine are the real design in small.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. An interpreter generates no machine code, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source   pick the Lox token stream of the example (stands in for the scanner)
 *   stage_compile  Pratt parser and code generator: tokens in, one function object per Lox
 *                  function out, each disassembled the moment it is finished
 *   stage_run      execute the script in the VM loop, printing the stack before each step
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------
 * Tokens
 * ---------------------------------------------------------------------------------------- */

enum token_type {
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_SEMICOLON,
    TOKEN_MINUS,
    TOKEN_PLUS,
    TOKEN_STAR,
    TOKEN_EQUAL,
    TOKEN_LESS,
    TOKEN_IDENTIFIER,
    TOKEN_STRING,
    TOKEN_NUMBER,
    TOKEN_ELSE,
    TOKEN_FUN,
    TOKEN_IF,
    TOKEN_PRINT,
    TOKEN_RETURN,
    TOKEN_VAR,
    TOKEN_WHILE,
    TOKEN_EOF,
    TOKEN_COUNT,
};

struct token {
    enum token_type type;
    const char *text;
};

#define T(type, text) {type, text}
#define LP T(TOKEN_LEFT_PAREN, "(")
#define RP T(TOKEN_RIGHT_PAREN, ")")
#define LB T(TOKEN_LEFT_BRACE, "{")
#define RB T(TOKEN_RIGHT_BRACE, "}")
#define SEMI T(TOKEN_SEMICOLON, ";")
#define PLUS T(TOKEN_PLUS, "+")
#define MINUS T(TOKEN_MINUS, "-")
#define STAR T(TOKEN_STAR, "*")
#define EQ T(TOKEN_EQUAL, "=")
#define LT T(TOKEN_LESS, "<")
#define ID(s) T(TOKEN_IDENTIFIER, s)
#define NUM(s) T(TOKEN_NUMBER, s)
#define PRINT T(TOKEN_PRINT, "print")
#define VAR T(TOKEN_VAR, "var")
#define EOT T(TOKEN_EOF, "")

/* print "Hello, world!"; */
static const struct token hello_tokens[] = {
    PRINT, T(TOKEN_STRING, "Hello, world!"), SEMI, EOT,
};

/* print (2 + 3) * 4 - 6; */
static const struct token arithmetic_tokens[] = {
    PRINT, LP, NUM("2"), PLUS, NUM("3"), RP, STAR, NUM("4"), MINUS, NUM("6"), SEMI, EOT,
};

/* { var a = 3; var b = 5; if (a < b) { print b; } else { print a; } } */
static const struct token branch_tokens[] = {
    LB,
    VAR, ID("a"), EQ, NUM("3"), SEMI,
    VAR, ID("b"), EQ, NUM("5"), SEMI,
    T(TOKEN_IF, "if"), LP, ID("a"), LT, ID("b"), RP,
    LB, PRINT, ID("b"), SEMI, RB,
    T(TOKEN_ELSE, "else"),
    LB, PRINT, ID("a"), SEMI, RB,
    RB, EOT,
};

/* { var sum = 0; var i = 1; while (i < 11) { sum = sum + i; i = i + 1; } print sum; } */
static const struct token loop_tokens[] = {
    LB,
    VAR, ID("sum"), EQ, NUM("0"), SEMI,
    VAR, ID("i"), EQ, NUM("1"), SEMI,
    T(TOKEN_WHILE, "while"), LP, ID("i"), LT, NUM("11"), RP,
    LB,
    ID("sum"), EQ, ID("sum"), PLUS, ID("i"), SEMI,
    ID("i"), EQ, ID("i"), PLUS, NUM("1"), SEMI,
    RB,
    PRINT, ID("sum"), SEMI,
    RB, EOT,
};

/* { fun fact(n) { if (n < 2) { return 1; } return n * fact(n - 1); } print fact(5); } */
static const struct token call_tokens[] = {
    LB,
    T(TOKEN_FUN, "fun"), ID("fact"), LP, ID("n"), RP,
    LB,
    T(TOKEN_IF, "if"), LP, ID("n"), LT, NUM("2"), RP,
    LB, T(TOKEN_RETURN, "return"), NUM("1"), SEMI, RB,
    T(TOKEN_RETURN, "return"), ID("n"), STAR, ID("fact"), LP, ID("n"), MINUS, NUM("1"), RP, SEMI,
    RB,
    PRINT, ID("fact"), LP, NUM("5"), RP, SEMI,
    RB, EOT,
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
 * Values and objects, from value.h and object.h
 *
 * Without a garbage collector, every object goes on one list that is freed at exit.
 * ---------------------------------------------------------------------------------------- */

enum obj_type { OBJ_CLOSURE, OBJ_FUNCTION, OBJ_STRING, OBJ_UPVALUE };

struct obj {
    enum obj_type type;
    struct obj *next;
};

enum value_type { VAL_BOOL, VAL_NIL, VAL_NUMBER, VAL_OBJ };

struct value {
    enum value_type type;
    union {
        bool boolean;
        double number;
        struct obj *obj;
    } as;
};

#define BOOL_VAL(v) ((struct value){VAL_BOOL, {.boolean = (v)}})
#define NIL_VAL ((struct value){VAL_NIL, {.number = 0}})
#define NUMBER_VAL(v) ((struct value){VAL_NUMBER, {.number = (v)}})
#define OBJ_VAL(o) ((struct value){VAL_OBJ, {.obj = (struct obj *)(o)}})

/* Bytecode of one function: fixed-size arrays instead of clox's growable ones */
struct chunk {
    int count;
    uint8_t code[128];
    int constant_count;
    struct value constants[16];
};

struct obj_string {
    struct obj obj;
    char *chars;
};

struct obj_function {
    struct obj obj;
    int arity;
    int upvalue_count;
    struct chunk chunk;
    struct obj_string *name; /* NULL for the top-level script */
};

struct obj_upvalue {
    struct obj obj;
    struct value *location; /* a stack slot while open, &closed afterwards */
    struct value closed;
    struct obj_upvalue *next;
};

struct obj_closure {
    struct obj obj;
    struct obj_function *function;
    struct obj_upvalue *upvalues[8];
};

static struct obj *all_objects = NULL;

static void *allocate_object(size_t size, enum obj_type type)
{
    struct obj *object = calloc(1, size);
    object->type = type;
    object->next = all_objects;
    all_objects = object;
    return object;
}

static struct obj_string *new_string(const char *text)
{
    struct obj_string *string = allocate_object(sizeof(*string), OBJ_STRING);
    string->chars = malloc(strlen(text) + 1);
    strcpy(string->chars, text);
    return string;
}

static struct obj_function *new_function(void)
{
    return allocate_object(sizeof(struct obj_function), OBJ_FUNCTION);
}

static struct obj_closure *new_closure(struct obj_function *function)
{
    struct obj_closure *closure = allocate_object(sizeof(*closure), OBJ_CLOSURE);
    closure->function = function;
    return closure;
}

static struct obj_upvalue *new_upvalue(struct value *slot)
{
    struct obj_upvalue *upvalue = allocate_object(sizeof(*upvalue), OBJ_UPVALUE);
    upvalue->location = slot;
    upvalue->closed = NIL_VAL;
    return upvalue;
}

static void free_objects(void)
{
    while (all_objects != NULL) {
        struct obj *next = all_objects->next;
        if (all_objects->type == OBJ_STRING) {
            free(((struct obj_string *)all_objects)->chars);
        }
        free(all_objects);
        all_objects = next;
    }
}

static void print_function(FILE *out, const struct obj_function *function)
{
    if (function->name == NULL) {
        fprintf(out, "<script>");
    } else {
        fprintf(out, "<fn %s>", function->name->chars);
    }
}

static void print_value(FILE *out, struct value value)
{
    switch (value.type) {
    case VAL_BOOL:
        fprintf(out, value.as.boolean ? "true" : "false");
        break;
    case VAL_NIL:
        fprintf(out, "nil");
        break;
    case VAL_NUMBER:
        fprintf(out, "%g", value.as.number);
        break;
    default: /* VAL_OBJ */
        switch (value.as.obj->type) {
        case OBJ_CLOSURE:
            print_function(out, ((struct obj_closure *)value.as.obj)->function);
            break;
        case OBJ_FUNCTION:
            print_function(out, (struct obj_function *)value.as.obj);
            break;
        default: /* OBJ_STRING: an upvalue is never a Lox value */
            fprintf(out, "%s", ((struct obj_string *)value.as.obj)->chars);
            break;
        }
        break;
    }
}

/* ------------------------------------------------------------------------------------------
 * Bytecode, from chunk.h
 *
 * Operands follow the opcode as single bytes. A jump carries a 16-bit offset in two bytes.
 * OP_CLOSURE is followed by one stack-slot byte per captured variable.
 * ---------------------------------------------------------------------------------------- */

enum opcode {
    OP_CONSTANT,
    OP_NIL,
    OP_POP,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_UPVALUE,
    OP_LESS,
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_PRINT,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CALL,
    OP_CLOSURE,
    OP_CLOSE_UPVALUE,
    OP_RETURN,
};

/* ------------------------------------------------------------------------------------------
 * Disassembler, from debug.c
 * ---------------------------------------------------------------------------------------- */

static int simple_instruction(const char *name, int offset)
{
    fprintf(stderr, "%s\n", name);
    return offset + 1;
}

static int byte_instruction(const char *name, const struct chunk *chunk, int offset)
{
    fprintf(stderr, "%-16s %4d\n", name, chunk->code[offset + 1]);
    return offset + 2;
}

static int jump_instruction(const char *name, int sign, const struct chunk *chunk, int offset)
{
    int jump = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
    fprintf(stderr, "%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

static int disassemble_instruction(const struct chunk *chunk, int offset)
{
    fprintf(stderr, "%04d ", offset);
    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
    case OP_CONSTANT: {
        uint8_t constant = chunk->code[offset + 1];
        fprintf(stderr, "%-16s %4d '", "OP_CONSTANT", constant);
        print_value(stderr, chunk->constants[constant]);
        fprintf(stderr, "'\n");
        return offset + 2;
    }
    case OP_NIL:
        return simple_instruction("OP_NIL", offset);
    case OP_POP:
        return simple_instruction("OP_POP", offset);
    case OP_GET_LOCAL:
        return byte_instruction("OP_GET_LOCAL", chunk, offset);
    case OP_SET_LOCAL:
        return byte_instruction("OP_SET_LOCAL", chunk, offset);
    case OP_GET_UPVALUE:
        return byte_instruction("OP_GET_UPVALUE", chunk, offset);
    case OP_LESS:
        return simple_instruction("OP_LESS", offset);
    case OP_ADD:
        return simple_instruction("OP_ADD", offset);
    case OP_SUBTRACT:
        return simple_instruction("OP_SUBTRACT", offset);
    case OP_MULTIPLY:
        return simple_instruction("OP_MULTIPLY", offset);
    case OP_PRINT:
        return simple_instruction("OP_PRINT", offset);
    case OP_JUMP:
        return jump_instruction("OP_JUMP", 1, chunk, offset);
    case OP_JUMP_IF_FALSE:
        return jump_instruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
    case OP_LOOP:
        return jump_instruction("OP_LOOP", -1, chunk, offset);
    case OP_CALL:
        return byte_instruction("OP_CALL", chunk, offset);
    case OP_CLOSURE: {
        uint8_t constant = chunk->code[offset + 1];
        fprintf(stderr, "%-16s %4d ", "OP_CLOSURE", constant);
        print_value(stderr, chunk->constants[constant]);
        fprintf(stderr, "\n");
        offset += 2;
        const struct obj_function *function = (struct obj_function *)chunk->constants[constant].as.obj;
        for (int i = 0; i < function->upvalue_count; i++) {
            fprintf(stderr, "%04d      |                     local %d\n", offset, chunk->code[offset]);
            offset++;
        }
        return offset;
    }
    case OP_CLOSE_UPVALUE:
        return simple_instruction("OP_CLOSE_UPVALUE", offset);
    default: /* OP_RETURN: the only opcode left */
        return simple_instruction("OP_RETURN", offset);
    }
}

static void disassemble_chunk(const struct obj_function *function)
{
    fprintf(stderr, "== ");
    print_function(stderr, function);
    fprintf(stderr, " ==\n");
    for (int offset = 0; offset < function->chunk.count;) {
        offset = disassemble_instruction(&function->chunk, offset);
    }
}

/* ------------------------------------------------------------------------------------------
 * Compiler, from compiler.c
 *
 * One struct compiler per function being compiled; they form a stack through `enclosing`.
 * ---------------------------------------------------------------------------------------- */

struct local {
    const char *name;
    int depth;
    bool is_captured; /* some closure captures this local, so scope exit must close it */
};

struct compiler {
    struct compiler *enclosing;
    struct obj_function *function;
    struct local locals[16];
    int local_count;
    uint8_t upvalues[8]; /* the enclosing function's stack slot that each upvalue captures */
    int scope_depth;
};

struct parser {
    const struct token *tokens;
    struct token current;
    struct token previous;
};

static struct parser parser;
static struct compiler *current = NULL;

static struct chunk *current_chunk(void)
{
    return &current->function->chunk;
}

static void advance(void)
{
    parser.previous = parser.current;
    parser.current = *parser.tokens++;
}

static bool check(enum token_type type)
{
    return parser.current.type == type;
}

static bool match(enum token_type type)
{
    if (!check(type)) {
        return false;
    }
    advance();
    return true;
}

/* The tokens are well formed, so a missing token is simply skipped over */
static void consume(enum token_type type)
{
    (void)type;
    advance();
}

static void emit_byte(uint8_t byte)
{
    struct chunk *chunk = current_chunk();
    chunk->code[chunk->count++] = byte;
}

static void emit_bytes(uint8_t byte1, uint8_t byte2)
{
    emit_byte(byte1);
    emit_byte(byte2);
}

static void emit_loop(int loop_start)
{
    emit_byte(OP_LOOP);
    int offset = current_chunk()->count - loop_start + 2;
    emit_byte((offset >> 8) & 0xff);
    emit_byte(offset & 0xff);
}

static int emit_jump(uint8_t instruction)
{
    emit_byte(instruction);
    emit_byte(0xff);
    emit_byte(0xff);
    return current_chunk()->count - 2;
}

static uint8_t make_constant(struct value value)
{
    struct chunk *chunk = current_chunk();
    chunk->constants[chunk->constant_count] = value;
    return (uint8_t)chunk->constant_count++;
}

static void patch_jump(int offset)
{
    int jump = current_chunk()->count - offset - 2;
    current_chunk()->code[offset] = (jump >> 8) & 0xff;
    current_chunk()->code[offset + 1] = jump & 0xff;
}

static void init_compiler(struct compiler *compiler, const char *name)
{
    memset(compiler, 0, sizeof(*compiler));
    compiler->enclosing = current;
    compiler->function = new_function();
    current = compiler;
    if (name != NULL) {
        current->function->name = new_string(name);
    }
    /* Slot zero of every frame holds the function being called */
    current->locals[current->local_count++] = (struct local){"", 0, false};
}

static struct obj_function *end_compiler(void)
{
    emit_bytes(OP_NIL, OP_RETURN);
    struct obj_function *function = current->function;
    disassemble_chunk(function);
    current = current->enclosing;
    return function;
}

static void begin_scope(void)
{
    current->scope_depth++;
}

static void end_scope(void)
{
    current->scope_depth--;
    while (current->locals[current->local_count - 1].depth > current->scope_depth) {
        if (current->locals[current->local_count - 1].is_captured) {
            emit_byte(OP_CLOSE_UPVALUE);
        } else {
            emit_byte(OP_POP);
        }
        current->local_count--;
    }
}

/* Returns the stack slot of a local of `compiler`, or -1 */
static int resolve_local(struct compiler *compiler, const char *name)
{
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        if (strcmp(compiler->locals[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * A name that is not a local of the current function can only be a local of the enclosing
 * one in these examples. Mark it captured and give the current function an upvalue for it.
 */
static int resolve_upvalue(struct compiler *compiler, const char *name)
{
    int slot = resolve_local(compiler->enclosing, name);
    compiler->enclosing->locals[slot].is_captured = true;
    int index = compiler->function->upvalue_count++;
    compiler->upvalues[index] = (uint8_t)slot;
    return index;
}

static void add_local(const char *name)
{
    current->locals[current->local_count++] = (struct local){name, current->scope_depth, false};
}

enum precedence {
    PREC_NONE,
    PREC_ASSIGNMENT,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_CALL,
    PREC_PRIMARY,
};

typedef void (*parse_fn)(void);

struct parse_rule {
    parse_fn prefix;
    parse_fn infix;
    enum precedence precedence;
};

static void expression(void);
static void statement(void);
static void declaration(void);
static const struct parse_rule *get_rule(enum token_type type);

static void parse_precedence(enum precedence precedence)
{
    advance();
    get_rule(parser.previous.type)->prefix();
    while (precedence <= get_rule(parser.current.type)->precedence) {
        advance();
        get_rule(parser.previous.type)->infix();
    }
}

static void expression(void)
{
    parse_precedence(PREC_ASSIGNMENT);
}

static void number(void)
{
    emit_bytes(OP_CONSTANT, make_constant(NUMBER_VAL(strtod(parser.previous.text, NULL))));
}

static void string(void)
{
    emit_bytes(OP_CONSTANT, make_constant(OBJ_VAL(new_string(parser.previous.text))));
}

static void grouping(void)
{
    expression();
    consume(TOKEN_RIGHT_PAREN);
}

/*
 * A name is a local of this function (OP_GET_LOCAL) or a variable of the enclosing function
 * (OP_GET_UPVALUE). Only locals can be assigned. The assignment is spotted by looking at the
 * next token, where clox threads a canAssign flag through the parse functions.
 */
static void variable(void)
{
    const char *name = parser.previous.text;
    int arg = resolve_local(current, name);
    if (arg != -1) {
        if (match(TOKEN_EQUAL)) {
            expression();
            emit_bytes(OP_SET_LOCAL, (uint8_t)arg);
        } else {
            emit_bytes(OP_GET_LOCAL, (uint8_t)arg);
        }
    } else {
        emit_bytes(OP_GET_UPVALUE, (uint8_t)resolve_upvalue(current, name));
    }
}

/* A call has exactly one argument */
static void call(void)
{
    expression();
    consume(TOKEN_RIGHT_PAREN);
    emit_bytes(OP_CALL, 1);
}

static void binary(void)
{
    enum token_type operator_type = parser.previous.type;
    const struct parse_rule *rule = get_rule(operator_type);
    parse_precedence((enum precedence)(rule->precedence + 1));
    switch (operator_type) {
    case TOKEN_LESS:
        emit_byte(OP_LESS);
        break;
    case TOKEN_PLUS:
        emit_byte(OP_ADD);
        break;
    case TOKEN_MINUS:
        emit_byte(OP_SUBTRACT);
        break;
    default: /* TOKEN_STAR: the only operator left */
        emit_byte(OP_MULTIPLY);
        break;
    }
}

static const struct parse_rule rules[TOKEN_COUNT] = {
    [TOKEN_LEFT_PAREN] = {grouping, call, PREC_CALL},
    [TOKEN_MINUS] = {NULL, binary, PREC_TERM},
    [TOKEN_PLUS] = {NULL, binary, PREC_TERM},
    [TOKEN_STAR] = {NULL, binary, PREC_FACTOR},
    [TOKEN_LESS] = {NULL, binary, PREC_COMPARISON},
    [TOKEN_IDENTIFIER] = {variable, NULL, PREC_NONE},
    [TOKEN_STRING] = {string, NULL, PREC_NONE},
    [TOKEN_NUMBER] = {number, NULL, PREC_NONE},
};

static const struct parse_rule *get_rule(enum token_type type)
{
    return &rules[type];
}

static void block(void)
{
    while (!check(TOKEN_RIGHT_BRACE)) {
        declaration();
    }
    consume(TOKEN_RIGHT_BRACE);
}

/* fun name(param) { body }: a new compiler, a new function object, an OP_CLOSURE */
static void fun_declaration(void)
{
    consume(TOKEN_IDENTIFIER);
    const char *name = parser.previous.text;
    add_local(name);

    struct compiler compiler;
    init_compiler(&compiler, name);
    begin_scope();
    consume(TOKEN_LEFT_PAREN);
    consume(TOKEN_IDENTIFIER);
    current->function->arity = 1;
    add_local(parser.previous.text);
    consume(TOKEN_RIGHT_PAREN);
    consume(TOKEN_LEFT_BRACE);
    block();

    struct obj_function *function = end_compiler();
    emit_bytes(OP_CLOSURE, make_constant(OBJ_VAL(function)));
    for (int i = 0; i < function->upvalue_count; i++) {
        emit_byte(compiler.upvalues[i]);
    }
}

static void var_declaration(void)
{
    consume(TOKEN_IDENTIFIER);
    add_local(parser.previous.text);
    consume(TOKEN_EQUAL);
    expression();
    consume(TOKEN_SEMICOLON);
}

static void print_statement(void)
{
    expression();
    consume(TOKEN_SEMICOLON);
    emit_byte(OP_PRINT);
}

static void expression_statement(void)
{
    expression();
    consume(TOKEN_SEMICOLON);
    emit_byte(OP_POP);
}

static void return_statement(void)
{
    expression();
    consume(TOKEN_SEMICOLON);
    emit_byte(OP_RETURN);
}

/*
 * if: the condition is left on the stack, OP_JUMP_IF_FALSE does not pop it, so each branch
 * starts with an OP_POP of its own.
 */
static void if_statement(void)
{
    consume(TOKEN_LEFT_PAREN);
    expression();
    consume(TOKEN_RIGHT_PAREN);

    int then_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();
    int else_jump = emit_jump(OP_JUMP);
    patch_jump(then_jump);
    emit_byte(OP_POP);
    if (match(TOKEN_ELSE)) {
        statement();
    }
    patch_jump(else_jump);
}

static void while_statement(void)
{
    int loop_start = current_chunk()->count;
    consume(TOKEN_LEFT_PAREN);
    expression();
    consume(TOKEN_RIGHT_PAREN);

    int exit_jump = emit_jump(OP_JUMP_IF_FALSE);
    emit_byte(OP_POP);
    statement();
    emit_loop(loop_start);
    patch_jump(exit_jump);
    emit_byte(OP_POP);
}

static void statement(void)
{
    if (match(TOKEN_PRINT)) {
        print_statement();
    } else if (match(TOKEN_IF)) {
        if_statement();
    } else if (match(TOKEN_RETURN)) {
        return_statement();
    } else if (match(TOKEN_WHILE)) {
        while_statement();
    } else if (match(TOKEN_LEFT_BRACE)) {
        begin_scope();
        block();
        end_scope();
    } else {
        expression_statement();
    }
}

static void declaration(void)
{
    if (match(TOKEN_FUN)) {
        fun_declaration();
    } else if (match(TOKEN_VAR)) {
        var_declaration();
    } else {
        statement();
    }
}

/* ------------------------------------------------------------------------------------------
 * Virtual machine, from vm.c
 * ---------------------------------------------------------------------------------------- */

struct call_frame {
    struct obj_closure *closure;
    const uint8_t *ip;
    struct value *slots; /* this frame's window into the value stack */
};

static struct call_frame frames[32];
static int frame_count = 0;
static struct value stack[256];
static struct value *stack_top = stack;
static struct obj_upvalue *open_upvalues = NULL;

static void push(struct value value)
{
    *stack_top++ = value;
}

static struct value pop(void)
{
    return *--stack_top;
}

static struct value peek(int distance)
{
    return stack_top[-1 - distance];
}

/* Only comparisons feed a condition here, so the value is always a bool */
static bool is_falsey(struct value value)
{
    return !value.as.boolean;
}

static void call_closure(struct obj_closure *closure, int arg_count)
{
    struct call_frame *frame = &frames[frame_count++];
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
    frame->slots = stack_top - arg_count - 1;
}

/* An open upvalue points at a live stack slot. New ones go on the front of the list. */
static struct obj_upvalue *capture_upvalue(struct value *local)
{
    struct obj_upvalue *created = new_upvalue(local);
    created->next = open_upvalues;
    open_upvalues = created;
    return created;
}

/* Close every open upvalue at or above `last`: copy the value out of the stack slot */
static void close_upvalues(const struct value *last)
{
    while (open_upvalues != NULL && open_upvalues->location >= last) {
        struct obj_upvalue *upvalue = open_upvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        open_upvalues = upvalue->next;
    }
}

static void trace_stack(void)
{
    fprintf(stderr, "          ");
    for (const struct value *slot = stack; slot < stack_top; slot++) {
        fprintf(stderr, "[ ");
        print_value(stderr, *slot);
        fprintf(stderr, " ]");
    }
    fprintf(stderr, "\n");
}

static void run(void)
{
    struct call_frame *frame = &frames[frame_count - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->closure->function->chunk.constants[READ_BYTE()])
#define BINARY_OP(value_type, op)                                                                  \
    do {                                                                                           \
        double b = pop().as.number;                                                                \
        double a = pop().as.number;                                                                \
        push(value_type(a op b));                                                                  \
    } while (0)

    for (;;) {
        trace_stack();
        disassemble_instruction(&frame->closure->function->chunk,
                                (int)(frame->ip - frame->closure->function->chunk.code));

        switch (READ_BYTE()) {
        case OP_CONSTANT:
            push(READ_CONSTANT());
            break;
        case OP_NIL:
            push(NIL_VAL);
            break;
        case OP_POP:
            pop();
            break;
        case OP_GET_LOCAL:
            push(frame->slots[READ_BYTE()]);
            break;
        case OP_SET_LOCAL:
            frame->slots[READ_BYTE()] = peek(0);
            break;
        case OP_GET_UPVALUE:
            push(*frame->closure->upvalues[READ_BYTE()]->location);
            break;
        case OP_LESS:
            BINARY_OP(BOOL_VAL, <);
            break;
        case OP_ADD:
            BINARY_OP(NUMBER_VAL, +);
            break;
        case OP_SUBTRACT:
            BINARY_OP(NUMBER_VAL, -);
            break;
        case OP_MULTIPLY:
            BINARY_OP(NUMBER_VAL, *);
            break;
        case OP_PRINT:
            print_value(stdout, pop());
            printf("\n");
            break;
        case OP_JUMP: {
            uint16_t offset = READ_SHORT();
            frame->ip += offset;
            break;
        }
        case OP_JUMP_IF_FALSE: {
            uint16_t offset = READ_SHORT();
            if (is_falsey(peek(0))) {
                frame->ip += offset;
            }
            break;
        }
        case OP_LOOP: {
            uint16_t offset = READ_SHORT();
            frame->ip -= offset;
            break;
        }
        case OP_CALL: {
            int arg_count = READ_BYTE();
            call_closure((struct obj_closure *)peek(arg_count).as.obj, arg_count);
            frame = &frames[frame_count - 1];
            break;
        }
        case OP_CLOSURE: {
            struct obj_function *function = (struct obj_function *)READ_CONSTANT().as.obj;
            struct obj_closure *closure = new_closure(function);
            push(OBJ_VAL(closure));
            for (int i = 0; i < function->upvalue_count; i++) {
                closure->upvalues[i] = capture_upvalue(frame->slots + READ_BYTE());
            }
            break;
        }
        case OP_CLOSE_UPVALUE:
            close_upvalues(stack_top - 1);
            pop();
            break;
        default: { /* OP_RETURN: the only opcode left */
            struct value result = pop();
            close_upvalues(frame->slots);
            frame_count--;
            if (frame_count == 0) {
                pop();
                return;
            }
            stack_top = frame->slots;
            push(result);
            frame = &frames[frame_count - 1];
            break;
        }
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef BINARY_OP
}

/* ------------------------------------------------------------------------------------------
 * Stages
 * ---------------------------------------------------------------------------------------- */

static const struct source *stage_source(const char *name)
{
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        if (strcmp(sources[i].name, name) == 0) {
            fprintf(stderr, "== stage_source: Lox tokens of %s ==\n", name);
            for (size_t t = 0; t + 1 < sources[i].count; t++) {
                fprintf(stderr, "%s%s", t == 0 ? "" : " ", sources[i].tokens[t].text);
            }
            fprintf(stderr, "\n");
            return &sources[i];
        }
    }
    return NULL;
}

static struct obj_function *stage_compile(const struct source *source)
{
    fprintf(stderr, "== stage_compile: bytecode, each function printed when it is finished ==\n");
    parser.tokens = source->tokens;
    struct compiler compiler;
    init_compiler(&compiler, NULL);
    advance();
    while (!check(TOKEN_EOF)) {
        declaration();
    }
    return end_compiler();
}

static void stage_run(struct obj_function *script)
{
    fprintf(stderr, "== stage_run: value stack, then the instruction about to run ==\n");
    struct obj_closure *closure = new_closure(script);
    push(OBJ_VAL(closure));
    call_closure(closure, 0);
    run();
}

int main(int argc, char **argv)
{
    const struct source *source = NULL;
    if (argc < 2 || (source = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }

    struct obj_function *script = stage_compile(source);
    stage_run(script);
    free_objects();
    return 0;
}
