/*
 * naive: the simplest compiler this repo can have.
 *
 * It takes a stack machine IR and expands every IR instruction into a fixed run of x86-64
 * bytes. There is no analysis, no optimization, no register allocation and no intermediate
 * form beyond the IR itself. Every value lives on the machine stack, and every local lives in
 * a stack slot. It exists as the floor that other designs are compared against.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the program's output on stdout, the intermediate form after each stage on stderr,
 *   and writes the final machine code to code.bin if a path is given.
 *
 * Stages:
 *   stage_select  pick the hardcoded IR program for the example
 *   stage_emit    expand each IR instruction into bytes, leaving jump offsets blank
 *   stage_patch   fill in the jump, call and string offsets
 *   stage_run     map the bytes as executable memory and call them
 */
#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------------------------------
 * The IR
 *
 * A program is a flat array of instructions. Jump and call arguments are indices into that
 * array. Operands live on a stack, and a function's locals are numbered slots, where slot 0
 * holds the function's one argument.
 *
 *   FUNC n      start a function with n local slots, which must be 1 to 15
 *   PUSH k      push the constant k
 *   ADD SUB MUL pop b, pop a, push a op b
 *   LT          pop b, pop a, push 1 if a < b, else 0
 *   LOAD s      push local slot s
 *   STORE s     pop into local slot s
 *   JMP t       continue at instruction t
 *   JZ t        pop, and continue at instruction t if it was 0
 *   CALL f      pop the argument, call the function at instruction f, push its result
 *   RET         pop the result and return it
 *   PRINT_INT   pop a non-negative integer and print it and a newline
 *   PRINT_STR   print the program's string
 * ---------------------------------------------------------------------------------------- */

enum op {
    OP_FUNC,
    OP_PUSH,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_LT,
    OP_LOAD,
    OP_STORE,
    OP_JMP,
    OP_JZ,
    OP_CALL,
    OP_RET,
    OP_PRINT_INT,
    OP_PRINT_STR,
};

static const char *const op_names[] = {
    "FUNC", "PUSH", "ADD", "SUB", "MUL", "LT", "LOAD", "STORE",
    "JMP", "JZ", "CALL", "RET", "PRINT_INT", "PRINT_STR",
};

struct ins {
    enum op op;
    int32_t arg;
};

struct program {
    const char *name;
    const struct ins *code;
    size_t length;
    const char *string; /* The text for PRINT_STR, or NULL if the program has none. */
};

/* print("Hello, world!") */
static const struct ins hello_code[] = {
    {OP_FUNC, 1},
    {OP_PRINT_STR, 0},
    {OP_PUSH, 0},
    {OP_RET, 0},
};

/* print((2 + 3) * 4 - 6) */
static const struct ins arithmetic_code[] = {
    {OP_FUNC, 1},
    {OP_PUSH, 2}, {OP_PUSH, 3}, {OP_ADD, 0},
    {OP_PUSH, 4}, {OP_MUL, 0},
    {OP_PUSH, 6}, {OP_SUB, 0},
    {OP_PRINT_INT, 0},
    {OP_PUSH, 0},
    {OP_RET, 0},
};

/*
 * local a = 3
 * local b = 5
 * if a < b then
 *   print(b)
 * else
 *   print(a)
 * end
 */
static const struct ins branch_code[] = {
    {OP_FUNC, 3},
    {OP_PUSH, 3}, {OP_STORE, 1},           /*  1 */
    {OP_PUSH, 5}, {OP_STORE, 2},           /*  3 */
    {OP_LOAD, 1}, {OP_LOAD, 2}, {OP_LT, 0}, /*  5 */
    {OP_JZ, 12},                           /*  8 */
    {OP_LOAD, 2}, {OP_PRINT_INT, 0},       /*  9 */
    {OP_JMP, 14},                          /* 11 */
    {OP_LOAD, 1}, {OP_PRINT_INT, 0},       /* 12 */
    {OP_PUSH, 0}, {OP_RET, 0},             /* 14 */
};

/*
 * local sum = 0
 * local i = 1
 * while i < 11 do
 *   sum = sum + i
 *   i = i + 1
 * end
 * print(sum)
 */
static const struct ins loop_code[] = {
    {OP_FUNC, 3},
    {OP_PUSH, 0}, {OP_STORE, 1},                          /*  1 */
    {OP_PUSH, 1}, {OP_STORE, 2},                          /*  3 */
    {OP_LOAD, 2}, {OP_PUSH, 11}, {OP_LT, 0}, {OP_JZ, 18}, /*  5 */
    {OP_LOAD, 1}, {OP_LOAD, 2}, {OP_ADD, 0}, {OP_STORE, 1}, /*  9 */
    {OP_LOAD, 2}, {OP_PUSH, 1}, {OP_ADD, 0}, {OP_STORE, 2}, /* 13 */
    {OP_JMP, 5},                                          /* 17 */
    {OP_LOAD, 1}, {OP_PRINT_INT, 0},                      /* 18 */
    {OP_PUSH, 0}, {OP_RET, 0},                            /* 20 */
};

/*
 * local function fact(n)
 *   if n < 2 then
 *     return 1
 *   end
 *   return n * fact(n - 1)
 * end
 * print(fact(5))
 */
static const struct ins call_code[] = {
    {OP_FUNC, 1},
    {OP_PUSH, 5}, {OP_CALL, 6}, {OP_PRINT_INT, 0},        /*  1 */
    {OP_PUSH, 0}, {OP_RET, 0},                            /*  4 */
    {OP_FUNC, 1},                                         /*  6: fact(n) */
    {OP_LOAD, 0}, {OP_PUSH, 2}, {OP_LT, 0}, {OP_JZ, 13},  /*  7 */
    {OP_PUSH, 1}, {OP_RET, 0},                            /* 11 */
    {OP_LOAD, 0},                                         /* 13 */
    {OP_LOAD, 0}, {OP_PUSH, 1}, {OP_SUB, 0}, {OP_CALL, 6}, /* 14 */
    {OP_MUL, 0},                                          /* 18 */
    {OP_RET, 0},                                          /* 19 */
};

#define PROGRAM(prog, text) {#prog, prog##_code, sizeof prog##_code / sizeof prog##_code[0], text}

static const struct program programs[] = {
    PROGRAM(hello, "Hello, world!\n"),
    PROGRAM(arithmetic, NULL),
    PROGRAM(branch, NULL),
    PROGRAM(loop, NULL),
    PROGRAM(call, NULL),
};

/* ------------------------------------------------------------------------------------------
 * Compiler state
 *
 * Everything is a fixed-size global, because the programs are hardcoded and small. AddressSanitizer
 * catches an overflow of any of these.
 * ---------------------------------------------------------------------------------------- */

#define PAGE_SIZE_BYTES 4096

static uint8_t code[PAGE_SIZE_BYTES];
static size_t code_length;

/* The code offset of each IR instruction, plus one extra entry for where the string goes. */
static size_t ir_offset[64];

/* A rel32 field whose value is only known once every instruction has been emitted. */
struct fixup {
    size_t at;     /* Code offset of the 4-byte field. */
    size_t target; /* IR index that the field must reach. */
};

static struct fixup fixups[32];
static size_t fixup_count;

static void emit(const uint8_t *bytes, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        code[code_length++] = bytes[i];
    }
}

#define EMIT(...) emit((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

static void emit32(uint32_t value)
{
    EMIT((uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24));
}

/* Emit a blank rel32 that stage_patch will point at the given IR instruction. */
static void emit_rel32(size_t target)
{
    fixups[fixup_count].at = code_length;
    fixups[fixup_count].target = target;
    fixup_count++;
    emit32(0);
}

/* Local slot s lives at [rbp - 8 * (s + 1)], which is a disp8 for slots 0 to 15. */
static uint8_t slot_disp(int32_t slot)
{
    return (uint8_t)(-8 * (slot + 1));
}

/* ------------------------------------------------------------------------------------------
 * stage_select
 * ---------------------------------------------------------------------------------------- */

static const struct program *stage_select(const char *name)
{
    for (size_t i = 0; i < sizeof programs / sizeof programs[0]; i++) {
        if (strcmp(programs[i].name, name) == 0) {
            fprintf(stderr, "== stage_select: IR of %s ==\n", name);
            for (size_t j = 0; j < programs[i].length; j++) {
                fprintf(stderr, "%3zu  %s %d\n", j, op_names[programs[i].code[j].op],
                        programs[i].code[j].arg);
            }
            return &programs[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------
 * stage_emit
 *
 * One template per IR instruction. The registers are used as scratch only, so nothing is
 * kept in a register between two IR instructions.
 * ---------------------------------------------------------------------------------------- */

static void stage_emit(const struct program *program)
{
    fprintf(stderr, "== stage_emit: bytes per IR instruction, offsets still blank ==\n");

    for (size_t i = 0; i < program->length; i++) {
        const struct ins cur = program->code[i];
        const size_t start = code_length;
        ir_offset[i] = start;

        switch (cur.op) {
        case OP_FUNC:
            /* push rbp; mov rbp, rsp; sub rsp, 8 * n; mov [rbp - 8], rdi */
            EMIT(0x55, 0x48, 0x89, 0xe5, 0x48, 0x81, 0xec);
            emit32((uint32_t)(8 * cur.arg));
            EMIT(0x48, 0x89, 0x7d, 0xf8);
            break;
        case OP_PUSH:
            /* push imm32 */
            EMIT(0x68);
            emit32((uint32_t)cur.arg);
            break;
        case OP_ADD:
            /* pop rcx; pop rax; add rax, rcx; push rax */
            EMIT(0x59, 0x58, 0x48, 0x01, 0xc8, 0x50);
            break;
        case OP_SUB:
            /* pop rcx; pop rax; sub rax, rcx; push rax */
            EMIT(0x59, 0x58, 0x48, 0x29, 0xc8, 0x50);
            break;
        case OP_MUL:
            /* pop rcx; pop rax; imul rax, rcx; push rax */
            EMIT(0x59, 0x58, 0x48, 0x0f, 0xaf, 0xc1, 0x50);
            break;
        case OP_LT:
            /* pop rcx; pop rax; cmp rax, rcx; setl al; movzx eax, al; push rax */
            EMIT(0x59, 0x58, 0x48, 0x39, 0xc8, 0x0f, 0x9c, 0xc0, 0x0f, 0xb6, 0xc0, 0x50);
            break;
        case OP_LOAD:
            /* push qword [rbp - 8 * (s + 1)] */
            EMIT(0xff, 0x75, slot_disp(cur.arg));
            break;
        case OP_STORE:
            /* pop qword [rbp - 8 * (s + 1)] */
            EMIT(0x8f, 0x45, slot_disp(cur.arg));
            break;
        case OP_JMP:
            /* jmp rel32 */
            EMIT(0xe9);
            emit_rel32((size_t)cur.arg);
            break;
        case OP_JZ:
            /* pop rax; test rax, rax; jz rel32 */
            EMIT(0x58, 0x48, 0x85, 0xc0, 0x0f, 0x84);
            emit_rel32((size_t)cur.arg);
            break;
        case OP_CALL:
            /* pop rdi; call rel32; push rax */
            EMIT(0x5f, 0xe8);
            emit_rel32((size_t)cur.arg);
            EMIT(0x50);
            break;
        case OP_RET:
            /* pop rax; leave; ret */
            EMIT(0x58, 0xc9, 0xc3);
            break;
        case OP_PRINT_INT:
            /*
             * pop rax; sub rsp, 32; lea rsi, [rsp + 32]; mov rcx, 10; dec rsi; mov byte [rsi], 10
             * loop: xor edx, edx; div rcx; add dl, '0'; dec rsi; mov [rsi], dl; test rax, rax;
             *       jnz loop
             * lea rdx, [rsp + 32]; sub rdx, rsi; mov eax, 1; mov edi, 1; syscall; add rsp, 32
             */
            EMIT(0x58, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x74, 0x24, 0x20,
                 0x48, 0xc7, 0xc1, 0x0a, 0x00, 0x00, 0x00, 0x48, 0xff, 0xce, 0xc6, 0x06, 0x0a,
                 0x31, 0xd2, 0x48, 0xf7, 0xf1, 0x80, 0xc2, 0x30, 0x48, 0xff, 0xce, 0x88, 0x16,
                 0x48, 0x85, 0xc0, 0x75, 0xee,
                 0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0x29, 0xf2, 0xb8, 0x01, 0x00, 0x00, 0x00,
                 0xbf, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x83, 0xc4, 0x20);
            break;
        case OP_PRINT_STR:
        default: /* No other value exists. Sharing the label keeps an unreachable branch out. */
            /* mov eax, 1; mov edi, 1; lea rsi, [rip + string]; mov edx, length; syscall */
            EMIT(0xb8, 0x01, 0x00, 0x00, 0x00, 0xbf, 0x01, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x35);
            emit_rel32(program->length);
            EMIT(0xba);
            emit32((uint32_t)strlen(program->string));
            EMIT(0x0f, 0x05);
            break;
        }

        fprintf(stderr, "%3zu  +0x%04zx  %-9s %2d  ", i, start, op_names[cur.op], cur.arg);
        for (size_t b = start; b < code_length; b++) {
            fprintf(stderr, " %02x", code[b]);
        }
        fprintf(stderr, "\n");
    }

    /* The string sits right after the last instruction, which is where the extra entry points. */
    ir_offset[program->length] = code_length;
    if (program->string != NULL) {
        emit((const uint8_t *)program->string, strlen(program->string));
    }
}

/* ------------------------------------------------------------------------------------------
 * stage_patch
 * ---------------------------------------------------------------------------------------- */

static void stage_patch(void)
{
    fprintf(stderr, "== stage_patch: resolved offsets ==\n");

    for (size_t i = 0; i < fixup_count; i++) {
        const struct fixup f = fixups[i];
        const int64_t target = (int64_t)ir_offset[f.target];
        const int64_t end_of_field = (int64_t)(f.at + 4);
        const uint32_t rel = (uint32_t)(int32_t)(target - end_of_field);

        code[f.at] = (uint8_t)rel;
        code[f.at + 1] = (uint8_t)(rel >> 8);
        code[f.at + 2] = (uint8_t)(rel >> 16);
        code[f.at + 3] = (uint8_t)(rel >> 24);
        fprintf(stderr, "+0x%04zx  rel32 %d  to IR %zu at +0x%04zx\n", f.at, (int32_t)rel,
                f.target, ir_offset[f.target]);
    }

    fprintf(stderr, "== stage_patch: final code, %zu bytes ==", code_length);
    for (size_t i = 0; i < code_length; i++) {
        if (i % 16 == 0) {
            fprintf(stderr, "\n+0x%04zx:", i);
        }
        fprintf(stderr, " %02x", code[i]);
    }
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------------------------------
 * stage_run
 * ---------------------------------------------------------------------------------------- */

static int64_t stage_run(void)
{
    fprintf(stderr, "== stage_run ==\n");

    void *buf = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (buf == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    // GCOVR_EXCL_STOP
    memcpy(buf, code, code_length);
    // GCOVR_EXCL_START: mapping or protecting one anonymous page does not fail
    if (mprotect(buf, PAGE_SIZE_BYTES, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        exit(1);
    }
    // GCOVR_EXCL_STOP

    /* ISO C forbids casting an object pointer to a function pointer, so copy the bits. */
    int64_t (*fn)(int64_t);
    memcpy(&fn, &buf, sizeof fn);
    const int64_t result = fn(0);

    fprintf(stderr, "returned %ld\n", (long)result);
    return result;
}

static void write_code(const char *path)
{
    FILE *f = fopen(path, "wb");
    // GCOVR_EXCL_START: the caller chose the path, and a bad one is the caller's problem
    if (f == NULL || fwrite(code, 1, code_length, f) != code_length || fclose(f) != 0) {
        perror(path);
        exit(1);
    }
    // GCOVR_EXCL_STOP
}

int main(int argc, char **argv)
{
    const struct program *program = NULL;
    if (argc < 2 || (program = stage_select(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }

    stage_emit(program);
    stage_patch();
    if (argc > 2) {
        write_code(argv[2]);
    }
    return (int)stage_run();
}
