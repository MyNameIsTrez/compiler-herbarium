/*
 * wren: a cut-down version of the Wren virtual machine.
 *
 * Wren has no operators in its bytecode. `a + b` is a method call: the compiler interns the
 * name `+(_)` (the name plus its arity) in one global symbol table, emits CALL_1 with that
 * symbol number, and at run time the VM takes the receiver's class and indexes the class's
 * method array with the symbol. There is no hashing and no inline cache. Calling a function
 * is a method call too (`fn.call(x)`), and the primitive behind it does not recurse in C: it
 * pushes a call frame on the fiber and the one interpreter loop keeps running.
 *
 * This entry skips the scanner and the parser. A function per example makes the same calls
 * the real compiler makes while it parses (intern a method symbol, emit CALL_n, emit and
 * patch a jump, define a module variable), and the VM, the class method tables and the
 * primitives are the real design in small.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout and the intermediate form after each stage on
 *   stderr. An interpreter generates no machine code, so the code.bin argument is ignored.
 *
 * Stages:
 *   stage_source   show the Wren source of the example (it is not parsed)
 *   stage_core     create the core classes and bind their primitives to method symbols
 *   stage_compile  emit bytecode with the compiler's calls, each function disassembled when
 *                  it is finished
 *   stage_run      execute the module on a fiber, printing the stack before each step
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CODE 96
#define MAX_CONSTANTS 16
#define MAX_SYMBOLS 16
#define MAX_VARIABLES 8
#define MAX_OBJECTS 16
#define STACK_SLOTS 64
#define MAX_FRAMES 16

/* ------------------------------------------------------------------------------------------
 * Values and objects
 * ---------------------------------------------------------------------------------------- */

enum value_type { VAL_FALSE, VAL_NULL, VAL_TRUE, VAL_NUM, VAL_OBJ };

struct value {
    enum value_type type;
    union {
        double num;
        struct obj *obj;
    } as;
};

enum obj_type { OBJ_CLASS, OBJ_CLOSURE, OBJ_FN, OBJ_STRING };

struct obj {
    enum obj_type type;
    struct obj_class *class_obj; /* every object has a class, a class has its metaclass */
};

enum method_type { METHOD_NONE, METHOD_PRIMITIVE, METHOD_FUNCTION_CALL };

struct method {
    enum method_type type;
    void (*primitive)(struct value *args);
};

/* A class holds one array of methods, indexed by method symbol, so a lookup is one load. A
 * symbol the class does not define is a hole of METHOD_NONE. */
struct obj_class {
    struct obj obj;
    const char *name;
    struct method methods[MAX_SYMBOLS];
};

struct obj_fn {
    struct obj obj;
    const char *name;
    unsigned char code[MAX_CODE];
    int code_count;
    struct value constants[MAX_CONSTANTS];
    int num_constants;
};

struct obj_closure {
    struct obj obj;
    struct obj_fn *fn;
};

struct obj_string {
    struct obj obj;
    const char *chars;
};

#define NUM_VAL(n) ((struct value){VAL_NUM, {.num = (n)}})
#define BOOL_VAL(b) ((struct value){(b) ? VAL_TRUE : VAL_FALSE, {.num = 0}})
#define NULL_VAL ((struct value){VAL_NULL, {.num = 0}})
#define OBJ_VAL(o) ((struct value){VAL_OBJ, {.obj = (struct obj *)(o)}})
#define AS_CLOSURE(v) ((struct obj_closure *)(v).as.obj)
#define AS_CLASS(v) ((struct obj_class *)(v).as.obj)

/* ------------------------------------------------------------------------------------------
 * The VM: symbol tables, core classes, module variables
 * ---------------------------------------------------------------------------------------- */

/* One table of method names for the whole VM. A name is the method name with its arity, as in
 * `call(_)`, so `call(_)` and `call(_,_)` are two symbols. The number of a name is its index
 * here, and is also the index into every class's method array. */
static const char *method_names[MAX_SYMBOLS];
static int num_method_names;

/* The variables of the module (the file). A top-level `var` is one of these, not a local. */
static const char *variable_names[MAX_VARIABLES];
static struct value variables[MAX_VARIABLES];
static int num_variables;

static struct obj_class *num_class;
static struct obj_class *fn_class;
static struct obj_class *system_class;

static struct obj *objects[MAX_OBJECTS];
static int num_objects;

static int method_symbol(const char *name)
{
    for (int i = 0; i < num_method_names; i++) {
        if (strcmp(method_names[i], name) == 0) {
            return i;
        }
    }
    method_names[num_method_names] = name;
    return num_method_names++;
}

static void *new_object(size_t size, enum obj_type type, struct obj_class *class_obj)
{
    struct obj *obj = calloc(1, size);
    obj->type = type;
    obj->class_obj = class_obj;
    objects[num_objects++] = obj;
    return obj;
}

static struct obj_class *new_class(const char *name, struct obj_class *metaclass)
{
    struct obj_class *class_obj = new_object(sizeof(*class_obj), OBJ_CLASS, metaclass);
    class_obj->name = name;
    return class_obj;
}

static struct obj_string *new_string(const char *chars)
{
    struct obj_string *string = new_object(sizeof(*string), OBJ_STRING, NULL);
    string->chars = chars;
    return string;
}

static void free_objects(void)
{
    for (int i = 0; i < num_objects; i++) {
        free(objects[i]);
    }
}

static int variable_symbol(const char *name)
{
    for (int i = 0; i < num_variables; i++) {
        if (strcmp(variable_names[i], name) == 0) {
            return i;
        }
    }
    variable_names[num_variables] = name;
    return num_variables++;
}

/* Binds a method to a class: the slot for the symbol is filled, the others stay holes. */
static void bind_method(struct obj_class *class_obj, const char *name, struct method method)
{
    int symbol = method_symbol(name);
    class_obj->methods[symbol] = method;
}

static void print_value(FILE *out, struct value value);

/* The primitives. They get the receiver in args[0] and the arguments after it, and leave the
 * result in args[0]. */
static void num_plus(struct value *args)
{
    args[0] = NUM_VAL(args[0].as.num + args[1].as.num);
}

static void num_minus(struct value *args)
{
    args[0] = NUM_VAL(args[0].as.num - args[1].as.num);
}

static void num_multiply(struct value *args)
{
    args[0] = NUM_VAL(args[0].as.num * args[1].as.num);
}

static void num_lt(struct value *args)
{
    args[0] = BOOL_VAL(args[0].as.num < args[1].as.num);
}

static void fn_new(struct value *args)
{
    /* The block argument is already a function, so Fn.new returns it. */
    args[0] = args[1];
}

static void system_print(struct value *args)
{
    print_value(stdout, args[1]);
    printf("\n");
    args[0] = args[1];
}

#define PRIMITIVE(class_obj, name, fn) \
    bind_method(class_obj, name, (struct method){METHOD_PRIMITIVE, fn})

/* ------------------------------------------------------------------------------------------
 * Bytecode
 * ---------------------------------------------------------------------------------------- */

/* name, and what follows the opcode: nothing, a constant, a module variable, a method symbol,
 * a forward jump, a backward jump or a closure. The order is the order in wren_opcodes.h. */
#define OPCODES(X)                \
    X(CONSTANT, K_CONSTANT)       \
    X(NULL, K_NONE)               \
    X(LOAD_LOCAL_0, K_NONE)       \
    X(LOAD_LOCAL_1, K_NONE)       \
    X(LOAD_LOCAL_2, K_NONE)       \
    X(LOAD_LOCAL_3, K_NONE)       \
    X(LOAD_LOCAL_4, K_NONE)       \
    X(LOAD_LOCAL_5, K_NONE)       \
    X(LOAD_LOCAL_6, K_NONE)       \
    X(LOAD_LOCAL_7, K_NONE)       \
    X(LOAD_LOCAL_8, K_NONE)       \
    X(LOAD_MODULE_VAR, K_VARIABLE) \
    X(STORE_MODULE_VAR, K_VARIABLE) \
    X(POP, K_NONE)                \
    X(CALL_0, K_SYMBOL)           \
    X(CALL_1, K_SYMBOL)           \
    X(CALL_2, K_SYMBOL)           \
    X(CALL_3, K_SYMBOL)           \
    X(CALL_4, K_SYMBOL)           \
    X(CALL_5, K_SYMBOL)           \
    X(CALL_6, K_SYMBOL)           \
    X(CALL_7, K_SYMBOL)           \
    X(CALL_8, K_SYMBOL)           \
    X(CALL_9, K_SYMBOL)           \
    X(CALL_10, K_SYMBOL)          \
    X(CALL_11, K_SYMBOL)          \
    X(CALL_12, K_SYMBOL)          \
    X(CALL_13, K_SYMBOL)          \
    X(CALL_14, K_SYMBOL)          \
    X(CALL_15, K_SYMBOL)          \
    X(CALL_16, K_SYMBOL)          \
    X(JUMP, K_FORWARD)            \
    X(LOOP, K_BACKWARD)           \
    X(JUMP_IF, K_FORWARD)         \
    X(RETURN, K_NONE)             \
    X(CLOSURE, K_CLOSURE)         \
    X(END_MODULE, K_NONE)         \
    X(END, K_NONE)

enum operand_kind {
    K_NONE,
    K_CONSTANT,
    K_VARIABLE,
    K_SYMBOL,
    K_FORWARD,
    K_BACKWARD,
    K_CLOSURE
};

enum opcode {
#define X(name, kind) CODE_##name,
    OPCODES(X)
#undef X
};

static const struct {
    const char *name;
    enum operand_kind kind;
} opcode_info[] = {
#define X(name, kind) {#name, kind},
    OPCODES(X)
#undef X
};

static void print_value(FILE *out, struct value value)
{
    switch (value.type) {
    case VAL_FALSE:
        fprintf(out, "false");
        break;
    case VAL_NULL:
        fprintf(out, "null");
        break;
    case VAL_TRUE:
        fprintf(out, "true");
        break;
    case VAL_NUM:
        fprintf(out, "%.14g", value.as.num);
        break;
    default: /* VAL_OBJ */
        if (value.as.obj->type == OBJ_STRING) {
            fprintf(out, "%s", ((struct obj_string *)value.as.obj)->chars);
        } else if (value.as.obj->type == OBJ_CLASS) {
            fprintf(out, "%s", AS_CLASS(value)->name);
        } else {
            fprintf(out, "<fn>");
        }
        break;
    }
}

/* Prints the instruction at offset and returns the offset of the next one. */
static int disassemble_instruction(const struct obj_fn *fn, int offset)
{
    enum opcode op = fn->code[offset];
    fprintf(stderr, "%04d %-16s", offset, opcode_info[op].name);
    int next = offset + 1;
    if (opcode_info[op].kind != K_NONE) {
        int arg = (fn->code[offset + 1] << 8) | fn->code[offset + 2];
        next = offset + 3;
        fprintf(stderr, " %d", arg);
        switch (opcode_info[op].kind) {
        case K_CONSTANT:
            fprintf(stderr, " '");
            print_value(stderr, fn->constants[arg]);
            fprintf(stderr, "'");
            break;
        case K_VARIABLE:
            fprintf(stderr, " '%s'", variable_names[arg]);
            break;
        case K_SYMBOL:
            fprintf(stderr, " '%s'", method_names[arg]);
            break;
        case K_FORWARD:
            fprintf(stderr, " -> %d", next + arg);
            break;
        case K_BACKWARD:
            fprintf(stderr, " -> %d", next - arg);
            break;
        default: /* K_CLOSURE; a closure with upvalues would add two bytes for each */
            fprintf(stderr, " <%s>", ((struct obj_fn *)fn->constants[arg].as.obj)->name);
            break;
        }
    }
    fprintf(stderr, "\n");
    return next;
}

static void disassemble_fn(const struct obj_fn *fn)
{
    fprintf(stderr, "== %s ==\n", fn->name);
    for (int offset = 0; offset < fn->code_count;) {
        offset = disassemble_instruction(fn, offset);
    }
}

/* ------------------------------------------------------------------------------------------
 * The compiler's building blocks
 *
 * Each function here is one the real compiler (wren_compiler.c) calls while it parses.
 * ---------------------------------------------------------------------------------------- */

struct compiler {
    struct obj_fn *fn;
    struct compiler *parent;
};

static void init_compiler(struct compiler *compiler, struct compiler *parent, const char *name)
{
    compiler->parent = parent;
    compiler->fn = new_object(sizeof(*compiler->fn), OBJ_FN, NULL);
    compiler->fn->name = name;
}

static int emit_byte(struct compiler *compiler, int byte)
{
    compiler->fn->code[compiler->fn->code_count] = (unsigned char)byte;
    return compiler->fn->code_count++;
}

static void emit_op(struct compiler *compiler, enum opcode op)
{
    emit_byte(compiler, op);
}

/* A 16-bit argument is written big endian. */
static void emit_short_arg(struct compiler *compiler, enum opcode op, int arg)
{
    emit_op(compiler, op);
    emit_byte(compiler, (arg >> 8) & 0xff);
    emit_byte(compiler, arg & 0xff);
}

/* Emits a jump with a placeholder offset and returns where the offset is. */
static int emit_jump(struct compiler *compiler, enum opcode op)
{
    emit_op(compiler, op);
    emit_byte(compiler, 0xff);
    return emit_byte(compiler, 0xff) - 1;
}

/* Points the jump at the current end of the code. The offset counts from just after the jump's
 * own argument. */
static void patch_jump(struct compiler *compiler, int offset)
{
    int jump = compiler->fn->code_count - offset - 2;
    compiler->fn->code[offset] = (unsigned char)((jump >> 8) & 0xff);
    compiler->fn->code[offset + 1] = (unsigned char)(jump & 0xff);
}

/* Adds a value to the function's constant table. */
static int add_constant(struct compiler *compiler, struct value value)
{
    struct obj_fn *fn = compiler->fn;
    fn->constants[fn->num_constants] = value;
    return fn->num_constants++;
}

/* A number reuses an equal constant that is already in the table. */
static int add_number_constant(struct compiler *compiler, double value)
{
    struct obj_fn *fn = compiler->fn;
    for (int i = 0; i < fn->num_constants; i++) {
        if (fn->constants[i].type == VAL_NUM && fn->constants[i].as.num == value) {
            return i;
        }
    }
    return add_constant(compiler, NUM_VAL(value));
}

static void emit_constant(struct compiler *compiler, struct value value)
{
    emit_short_arg(compiler, CODE_CONSTANT, add_constant(compiler, value));
}

static void number(struct compiler *compiler, double value)
{
    emit_short_arg(compiler, CODE_CONSTANT, add_number_constant(compiler, value));
}

static void string(struct compiler *compiler, const char *chars)
{
    emit_constant(compiler, OBJ_VAL(new_string(chars)));
}

/* A name that is not a local: a variable of the module, such as `System` or a `var`. */
static void load_variable(struct compiler *compiler, const char *name)
{
    emit_short_arg(compiler, CODE_LOAD_MODULE_VAR, variable_symbol(name));
}

static void load_local(struct compiler *compiler, int slot)
{
    emit_op(compiler, CODE_LOAD_LOCAL_0 + slot);
}

static void store_variable(struct compiler *compiler, const char *name)
{
    emit_short_arg(compiler, CODE_STORE_MODULE_VAR, variable_symbol(name));
}

/* `var name = <expression>` at the top level: store the value, then drop the temporary. */
static void define_variable(struct compiler *compiler, const char *name)
{
    store_variable(compiler, name);
    emit_op(compiler, CODE_POP);
}

/* The receiver and the arguments are already on the stack. */
static void call_method(struct compiler *compiler, int num_args, const char *name)
{
    emit_short_arg(compiler, CODE_CALL_0 + num_args, method_symbol(name));
}

/* An infix operator is a call of the method named after it, with one argument. */
static void infix_op(struct compiler *compiler, const char *name)
{
    call_method(compiler, 1, name);
}

/* An expression statement leaves nothing on the stack. */
static void end_statement(struct compiler *compiler)
{
    emit_op(compiler, CODE_POP);
}

static void emit_loop(struct compiler *compiler, int loop_start)
{
    /* The loop starts one byte before the current end, as in startLoop. */
    int offset = compiler->fn->code_count - loop_start + 2;
    emit_short_arg(compiler, CODE_LOOP, offset);
}

/* Finishes the function: marks the end, disassembles it, and in the enclosing function loads
 * the closure for it. Every function is wrapped in a closure, even without upvalues. */
static struct obj_fn *end_compiler(struct compiler *compiler)
{
    emit_op(compiler, CODE_END);
    disassemble_fn(compiler->fn);
    if (compiler->parent != NULL) {
        int constant = add_constant(compiler->parent, OBJ_VAL(compiler->fn));
        emit_short_arg(compiler->parent, CODE_CLOSURE, constant);
    }
    return compiler->fn;
}

/* The end of a module: END_MODULE leaves a null on the stack, and RETURN returns it. */
static void end_module(struct compiler *compiler)
{
    emit_op(compiler, CODE_END_MODULE);
    emit_op(compiler, CODE_RETURN);
}

/* ------------------------------------------------------------------------------------------
 * The fiber and the interpreter loop
 * ---------------------------------------------------------------------------------------- */

struct call_frame {
    const unsigned char *ip;
    struct obj_closure *closure;
    struct value *stack_start;
};

/* A fiber owns a value stack and a stack of call frames, so the interpreter loop never
 * recurses in C. This entry only has the root fiber. */
struct fiber {
    struct value stack[STACK_SLOTS];
    struct value *stack_top;
    struct call_frame frames[MAX_FRAMES];
    int num_frames;
};

static struct fiber fiber;

static void call_function(struct obj_closure *closure, int num_args)
{
    struct call_frame *frame = &fiber.frames[fiber.num_frames++];
    frame->stack_start = fiber.stack_top - num_args;
    frame->closure = closure;
    frame->ip = closure->fn->code;
}

static void trace_stack(void)
{
    fprintf(stderr, "          ");
    for (struct value *slot = fiber.stack; slot < fiber.stack_top; slot++) {
        fprintf(stderr, "[ ");
        print_value(stderr, *slot);
        fprintf(stderr, " ]");
    }
    fprintf(stderr, "\n");
}

static void run(void)
{
    struct call_frame *frame;
    struct value *stack_start;
    const unsigned char *ip;
    struct obj_fn *fn;

#define PUSH(value) (*fiber.stack_top++ = (value))
#define POP() (*(--fiber.stack_top))
#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (ip[-2] << 8) | ip[-1])
/* Store the instruction pointer before a frame is pushed, and reload after a frame is pushed
 * or popped. The locals keep the hot values in registers. */
#define STORE_FRAME() (frame->ip = ip)
#define LOAD_FRAME()                                       \
    do {                                                   \
        frame = &fiber.frames[fiber.num_frames - 1];       \
        stack_start = frame->stack_start;                  \
        ip = frame->ip;                                    \
        fn = frame->closure->fn;                           \
    } while (0)

    LOAD_FRAME();
    for (;;) {
        trace_stack();
        disassemble_instruction(fn, (int)(ip - fn->code));
        enum opcode instruction = READ_BYTE();

        if (instruction >= CODE_LOAD_LOCAL_0 && instruction <= CODE_LOAD_LOCAL_8) {
            PUSH(stack_start[instruction - CODE_LOAD_LOCAL_0]);
            continue;
        }

        if (instruction >= CODE_CALL_0 && instruction <= CODE_CALL_16) {
            /* Add one for the receiver, which is the first argument. */
            int num_args = instruction - CODE_CALL_0 + 1;
            int symbol = READ_SHORT();
            struct value *args = fiber.stack_top - num_args;
            struct obj_class *class_obj =
                args[0].type == VAL_NUM ? num_class : args[0].as.obj->class_obj;

            /* The whole method lookup: one index into the class's array. */
            struct method *method = &class_obj->methods[symbol];
            if (method->type == METHOD_PRIMITIVE) {
                method->primitive(args);
                /* The result is in the first slot. Drop the other arguments. */
                fiber.stack_top -= num_args - 1;
            } else {
                /* METHOD_FUNCTION_CALL: push a frame, and the loop continues in it. */
                STORE_FRAME();
                call_function(AS_CLOSURE(args[0]), num_args);
                LOAD_FRAME();
            }
            continue;
        }

        switch (instruction) { /* GCOVR_EXCL_BR_LINE: the default case is never run */
        case CODE_CONSTANT:
            PUSH(fn->constants[READ_SHORT()]);
            break;
        case CODE_LOAD_MODULE_VAR:
            PUSH(variables[READ_SHORT()]);
            break;
        case CODE_STORE_MODULE_VAR:
            variables[READ_SHORT()] = fiber.stack_top[-1];
            break;
        case CODE_POP:
            fiber.stack_top--;
            break;
        case CODE_JUMP: {
            int offset = READ_SHORT();
            ip += offset;
            break;
        }
        case CODE_LOOP: {
            int offset = READ_SHORT();
            ip -= offset;
            break;
        }
        case CODE_JUMP_IF: {
            int offset = READ_SHORT();
            struct value condition = POP();
            if (condition.type == VAL_FALSE) {
                ip += offset;
            }
            break;
        }
        case CODE_CLOSURE: {
            struct obj_closure *closure =
                new_object(sizeof(*closure), OBJ_CLOSURE, fn_class);
            closure->fn = (struct obj_fn *)fn->constants[READ_SHORT()].as.obj;
            PUSH(OBJ_VAL(closure));
            break;
        }
        case CODE_END_MODULE:
            PUSH(NULL_VAL);
            break;
        case CODE_RETURN: {
            struct value result = POP();
            fiber.num_frames--;
            if (fiber.num_frames == 0) {
                /* The fiber is done. */
                fiber.stack[0] = result;
                fiber.stack_top = fiber.stack + 1;
                return;
            }
            /* The result goes in the first slot, where the caller expects it, and the rest of
             * the callee's slots are dropped. */
            stack_start[0] = result;
            fiber.stack_top = frame->stack_start + 1;
            LOAD_FRAME();
            break;
        }
        default: /* GCOVR_EXCL_LINE: compiled but never run here: NULL, END, the rare loads */
            abort(); /* GCOVR_EXCL_LINE: see above */
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * The examples, as the compiler would emit them
 * ---------------------------------------------------------------------------------------- */

static struct obj_fn *compile_hello(void)
{
    struct compiler module;
    init_compiler(&module, NULL, "<module>");
    load_variable(&module, "System");
    string(&module, "Hello, world!");
    call_method(&module, 1, "print(_)");
    end_statement(&module);
    end_module(&module);
    return end_compiler(&module);
}

static struct obj_fn *compile_arithmetic(void)
{
    struct compiler module;
    init_compiler(&module, NULL, "<module>");
    load_variable(&module, "System");
    number(&module, 2);
    number(&module, 3);
    infix_op(&module, "+(_)");
    number(&module, 4);
    infix_op(&module, "*(_)");
    number(&module, 6);
    infix_op(&module, "-(_)");
    call_method(&module, 1, "print(_)");
    end_statement(&module);
    end_module(&module);
    return end_compiler(&module);
}

static struct obj_fn *compile_branch(void)
{
    struct compiler module;
    init_compiler(&module, NULL, "<module>");
    number(&module, 3);
    define_variable(&module, "a");
    number(&module, 5);
    define_variable(&module, "b");

    load_variable(&module, "a");
    load_variable(&module, "b");
    infix_op(&module, "<(_)");
    int if_jump = emit_jump(&module, CODE_JUMP_IF);
    load_variable(&module, "System");
    load_variable(&module, "b");
    call_method(&module, 1, "print(_)");
    end_statement(&module);
    int else_jump = emit_jump(&module, CODE_JUMP);
    patch_jump(&module, if_jump);
    load_variable(&module, "System");
    load_variable(&module, "a");
    call_method(&module, 1, "print(_)");
    end_statement(&module);
    patch_jump(&module, else_jump);

    end_module(&module);
    return end_compiler(&module);
}

static struct obj_fn *compile_loop(void)
{
    struct compiler module;
    init_compiler(&module, NULL, "<module>");
    number(&module, 0);
    define_variable(&module, "sum");
    number(&module, 1);
    define_variable(&module, "i");

    int loop_start = module.fn->code_count - 1;
    load_variable(&module, "i");
    number(&module, 11);
    infix_op(&module, "<(_)");
    int exit_jump = emit_jump(&module, CODE_JUMP_IF);
    load_variable(&module, "sum");
    load_variable(&module, "i");
    infix_op(&module, "+(_)");
    store_variable(&module, "sum");
    end_statement(&module);
    load_variable(&module, "i");
    number(&module, 1);
    infix_op(&module, "+(_)");
    store_variable(&module, "i");
    end_statement(&module);
    emit_loop(&module, loop_start);
    patch_jump(&module, exit_jump);

    load_variable(&module, "System");
    load_variable(&module, "sum");
    call_method(&module, 1, "print(_)");
    end_statement(&module);
    end_module(&module);
    return end_compiler(&module);
}

static struct obj_fn *compile_call(void)
{
    struct compiler module;
    init_compiler(&module, NULL, "<module>");

    /* var fact = Fn.new {|n| ... }: the block is a function compiled inside the module. Its
     * parameter n is local slot 1, because slot 0 holds the function being called. `fact` is
     * not a local of the module, so the block reads it as a module variable and needs no
     * upvalue. */
    load_variable(&module, "Fn");
    struct compiler block;
    init_compiler(&block, &module, "<fn>");
    load_local(&block, 1);
    number(&block, 2);
    infix_op(&block, "<(_)");
    int if_jump = emit_jump(&block, CODE_JUMP_IF);
    number(&block, 1);
    emit_op(&block, CODE_RETURN);
    patch_jump(&block, if_jump);
    load_local(&block, 1);
    load_variable(&block, "fact");
    load_local(&block, 1);
    number(&block, 1);
    infix_op(&block, "-(_)");
    call_method(&block, 1, "call(_)");
    infix_op(&block, "*(_)");
    emit_op(&block, CODE_RETURN);
    /* The body ended in a statement, so it also returns null. That is never reached. */
    emit_op(&block, CODE_NULL);
    emit_op(&block, CODE_RETURN);
    end_compiler(&block);
    call_method(&module, 1, "new(_)");
    define_variable(&module, "fact");

    load_variable(&module, "System");
    load_variable(&module, "fact");
    number(&module, 5);
    call_method(&module, 1, "call(_)");
    call_method(&module, 1, "print(_)");
    end_statement(&module);
    end_module(&module);
    return end_compiler(&module);
}

struct example {
    const char *name;
    const char *source;
    struct obj_fn *(*compile)(void);
};

static const struct example examples[] = {
    {"arithmetic", "System.print((2 + 3) * 4 - 6)", compile_arithmetic},
    {"branch",
     "var a = 3\nvar b = 5\nif (a < b) {\n  System.print(b)\n} else {\n  System.print(a)\n}",
     compile_branch},
    {"call",
     "var fact = Fn.new {|n|\n  if (n < 2) return 1\n  return n * fact.call(n - 1)\n}\n"
     "System.print(fact.call(5))",
     compile_call},
    {"hello", "System.print(\"Hello, world!\")", compile_hello},
    {"loop",
     "var sum = 0\nvar i = 1\nwhile (i < 11) {\n  sum = sum + i\n  i = i + 1\n}\n"
     "System.print(sum)",
     compile_loop},
};

/* ------------------------------------------------------------------------------------------
 * Stages
 * ---------------------------------------------------------------------------------------- */

static const struct example *stage_source(const char *name)
{
    for (size_t i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) {
        if (strcmp(examples[i].name, name) == 0) {
            fprintf(stderr, "== stage_source: Wren source of %s ==\n%s\n", name,
                    examples[i].source);
            return &examples[i];
        }
    }
    return NULL;
}

static void dump_class(const struct obj_class *class_obj)
{
    fprintf(stderr, "%s:\n", class_obj->name);
    for (int symbol = 0; symbol < num_method_names; symbol++) {
        const char *kind[] = {"-", "primitive", "function call"};
        fprintf(stderr, "  [%d] %s\n", symbol, kind[class_obj->methods[symbol].type]);
    }
}

/* Creates the core classes and binds their primitives. Every bound name gets the next free
 * symbol, so a class that does not define a symbol has a hole in its array. */
static void stage_core(void)
{
    struct obj_class *fn_meta = new_class("Fn metaclass", NULL);
    struct obj_class *system_meta = new_class("System metaclass", NULL);
    num_class = new_class("Num", NULL);
    fn_class = new_class("Fn", fn_meta);
    system_class = new_class("System", system_meta);

    PRIMITIVE(fn_meta, "new(_)", fn_new);
    /* Fn.call(_) has no primitive: the loop pushes a call frame for it. */
    bind_method(fn_class, "call(_)", (struct method){METHOD_FUNCTION_CALL, NULL});
    PRIMITIVE(system_meta, "print(_)", system_print);
    PRIMITIVE(num_class, "+(_)", num_plus);
    PRIMITIVE(num_class, "-(_)", num_minus);
    PRIMITIVE(num_class, "*(_)", num_multiply);
    PRIMITIVE(num_class, "<(_)", num_lt);

    /* The core classes are variables of every module. */
    variables[variable_symbol("System")] = OBJ_VAL(system_class);
    variables[variable_symbol("Fn")] = OBJ_VAL(fn_class);

    fprintf(stderr, "== stage_core: method symbols, and each class's method array ==\n");
    for (int symbol = 0; symbol < num_method_names; symbol++) {
        fprintf(stderr, "symbol %d: %s\n", symbol, method_names[symbol]);
    }
    dump_class(num_class);
    dump_class(fn_class);
    dump_class(fn_meta);
    dump_class(system_meta);
}

static struct obj_fn *stage_compile(const struct example *example)
{
    fprintf(stderr, "== stage_compile: bytecode, each function printed when it is finished ==\n");
    return example->compile();
}

static void stage_run(struct obj_fn *module)
{
    fprintf(stderr, "== stage_run: value stack, then the instruction about to run ==\n");
    struct obj_closure *closure = new_object(sizeof(*closure), OBJ_CLOSURE, fn_class);
    closure->fn = module;
    fiber.stack_top = fiber.stack;
    *fiber.stack_top++ = OBJ_VAL(closure);
    call_function(closure, 1);
    run();
}

int main(int argc, char **argv)
{
    const struct example *example = NULL;
    if (argc < 2 || (example = stage_source(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }

    stage_core();
    struct obj_fn *module = stage_compile(example);
    stage_run(module);
    free_objects();
    return 0;
}
