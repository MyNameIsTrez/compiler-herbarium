/*
 * gso: a cut-down version of the GNU superoptimizer (superopt 2.5).
 *
 * A superoptimizer does not translate a program. It is given a function and searches for the
 * shortest instruction sequence that computes it. GSO does this by exhaustive iterative
 * deepening: it tries every sequence of length 1, then every one of length 2, and so on, and
 * stops at the first length that has an answer. The function (the "goal") is only ever used as
 * an oracle that says what the output should be for an input. There is no IR and no rules.
 *
 * Trying all sequences is only possible because of the prunes and the test:
 *   - an instruction may only read the inputs, constants or the results of earlier
 *     instructions, and every instruction writes a new register,
 *   - an operation that reads the carry flag is only tried after something has set it,
 *   - the last instruction must consume the result of the one before it, or else that one
 *     was useless and a shorter sequence exists,
 *   - at a leaf the output is compared with the goal on a single input first, which rejects
 *     nearly everything, and only then is the whole set of test inputs run.
 * There is no proof of equivalence. A sequence that passes the tests is trusted, which GSO's
 * README admits can be wrong with a very small probability.
 *
 * The toy instruction set copies the generic operations GSO defines for every target, with
 * the carry flag that produces its surprising results, such as `(a == 0)` in two instructions.
 *
 * Usage: ./a.out <kernel>
 *   Prints, for every test vector, the output of the program that the search found, on stdout.
 *   Prints the goal, the search and the sequences it found on stderr.
 *
 * Stages:
 *   stage_goal    pick the goal function and build the test vectors
 *   stage_search  iterative deepening over sequences until one passes the tests
 *   stage_verify  run the first sequence found on every vector, then on a held-out set
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t word;

#define MAX_INPUTS 2
#define MAX_INSNS 5
#define MAX_REGS (MAX_INPUTS + MAX_INSNS)
#define MAX_VECS 64
#define HELD_OUT 1000

/* ------------------------------------------------------------------------------------------
 * The instruction set. The carry flag is 0 or 1, and -1 while nothing has set it yet, which is
 * what lets the search skip carry readers. `sub.co` leaves carry set when there is no borrow,
 * like the sf of the IBM RS/6000 that the GSO paper uses.
 * ---------------------------------------------------------------------------------------- */

enum {
    OP_ADD,
    OP_SUB,
    OP_AND,
    OP_IOR,
    OP_XOR,
    OP_ANDC,
    OP_NOR,
    OP_ADD_CO,
    OP_SUB_CO,
    OP_ADD_CI,
    OP_SUB_CI,
    OP_LSHIFTR,
    OP_ASHIFTR,
};

static const char *const op_names[] = {
    "add",    "sub",    "and",    "ior",    "xor",     "andc",    "nor",
    "add.co", "sub.co", "add.ci", "sub.ci", "lshiftr", "ashiftr",
};

static int reads_carry(int op)
{
    return op == OP_ADD_CI || op == OP_SUB_CI;
}

static int sets_carry(int op)
{
    return op == OP_ADD_CO || op == OP_SUB_CO;
}

/* Perform one operation. *co is the carry afterwards, which is the carry before unless the
 * operation sets it. */
static word exec(int op, word a, word b, int ci, int *co)
{
    *co = ci;
    switch (op) {
    case OP_ADD:
        return a + b;
    case OP_SUB:
        return a - b;
    case OP_AND:
        return a & b;
    case OP_IOR:
        return a | b;
    case OP_XOR:
        return a ^ b;
    case OP_ANDC:
        return a & ~b;
    case OP_NOR:
        return ~(a | b);
    case OP_ADD_CO:
        *co = a + b < a;
        return a + b;
    case OP_SUB_CO:
        *co = a >= b;
        return a - b;
    case OP_ADD_CI:
        return a + b + (word)ci;
    case OP_SUB_CI:
        return a + ~b + (word)ci;
    case OP_LSHIFTR:
        return a >> (b & 31);
    default: /* OP_ASHIFTR */
        return (word)((int32_t)a >> (b & 31));
    }
}

/* An operand is a register number, or 100 plus an index into consts. */
enum { C0 = 100, C1, CM1, C31 };
static const word consts[] = {0, 1, 0xffffffffu, 31};

struct insn {
    int op, s1, s2, d;
};

static word fetch(const word *regs, int operand)
{
    return operand < C0 ? regs[operand] : consts[operand - C0];
}

static void print_operand(int operand)
{
    if (operand < C0) {
        fprintf(stderr, "r%d", operand);
    } else {
        fprintf(stderr, "#%d", (int)consts[operand - C0]);
    }
}

/* Run a sequence the way the search will not: instruction by instruction, from the inputs. */
static word run_program(const struct insn *prog, int n, const word *in, int arity)
{
    word regs[MAX_REGS];
    int ci = 0;
    memcpy(regs, in, sizeof(word) * (size_t)arity);
    for (int i = 0; i < n; i++) {
        regs[prog[i].d] = exec(prog[i].op, fetch(regs, prog[i].s1), fetch(regs, prog[i].s2), ci, &ci);
    }
    return regs[prog[n - 1].d];
}

/* ------------------------------------------------------------------------------------------
 * Goal functions. GSO writes each one as a line of C in goal.def.
 * ---------------------------------------------------------------------------------------- */

struct goal {
    const char *name;
    int arity;
    const char *text;
    word (*eval)(const word *v);
};

static word goal_eq0(const word *v)
{
    return v[0] == 0;
}

static word goal_ges0(const word *v)
{
    return (int32_t)v[0] >= 0;
}

static word goal_abs(const word *v)
{
    return (int32_t)v[0] < 0 ? 0 - v[0] : v[0];
}

static word goal_sign(const word *v)
{
    return (int32_t)v[0] > 0 ? 1 : (int32_t)v[0] < 0 ? 0xffffffffu : 0;
}

static word goal_minu(const word *v)
{
    return v[0] < v[1] ? v[0] : v[1];
}

static word goal_maxu(const word *v)
{
    return v[0] > v[1] ? v[0] : v[1];
}

static const struct goal goals[] = {
    {"abs", 1, "r = (signed_word) v0 < 0 ? -v0 : v0", goal_abs},
    {"eq0", 1, "r = v0 == 0", goal_eq0},
    {"ges0", 1, "r = (signed_word) v0 >= 0", goal_ges0},
    {"maxu", 2, "r = (unsigned_word) v0 > (unsigned_word) v1 ? v0 : v1", goal_maxu},
    {"minu", 2, "r = (unsigned_word) v0 < (unsigned_word) v1 ? v0 : v1", goal_minu},
    {"sign", 1, "r = (signed_word) v0 > 0 ? 1 : (signed_word) v0 < 0 ? -1 : 0", goal_sign},
};

/* ------------------------------------------------------------------------------------------
 * Test vectors: every combination of a few edge values, then pseudo-random words. The search
 * and stdout use the same ones. A second, larger random set is used once at the end.
 * ---------------------------------------------------------------------------------------- */

struct vec {
    word in[MAX_INPUTS];
    word out;
};

static const word edge[] = {0, 1, 2, 0x7fffffffu, 0x80000000u, 0xffffffffu};
#define N_EDGE 6
#define N_RANDOM 16

static word lcg_state;

static word lcg(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

static const struct goal *goal;
static struct vec vecs[MAX_VECS];
static int order[MAX_VECS]; /* the order in which test_sequence tries the vectors */
static int nvecs;

static void build_vectors(void)
{
    int combos = 1;
    for (int i = 0; i < goal->arity; i++) {
        combos *= N_EDGE;
    }
    for (int k = 0; k < combos; k++) {
        int rest = k;
        for (int i = 0; i < goal->arity; i++) {
            vecs[nvecs].in[i] = edge[rest % N_EDGE];
            rest /= N_EDGE;
        }
        nvecs++;
    }
    lcg_state = 1;
    for (int k = 0; k < N_RANDOM; k++) {
        for (int i = 0; i < goal->arity; i++) {
            vecs[nvecs].in[i] = lcg();
        }
        nvecs++;
    }
    for (int k = 0; k < nvecs; k++) {
        vecs[k].out = goal->eval(vecs[k].in);
        order[k] = k;
    }
}

/* ------------------------------------------------------------------------------------------
 * The search state. Registers 0 to arity-1 hold the inputs, and instruction n writes register
 * arity+n, so no register is ever overwritten.
 * ---------------------------------------------------------------------------------------- */

enum { CY_JUST_SET = 1 };

static struct insn seq[MAX_INSNS];
static word values[MAX_REGS];
static word goal_value;
static struct insn first[MAX_INSNS];
static int first_len;
static int nfound;
static long n_nodes, n_leaves, n_candidates;

static void print_insn(const struct insn *in)
{
    fprintf(stderr, "    r%d = %s ", in->d, op_names[in->op]);
    print_operand(in->s1);
    fprintf(stderr, ", ");
    print_operand(in->s2);
    fprintf(stderr, "\n");
}

/* A sequence matched the goal on the first input. Run it on all of them. A vector that rejects
 * a sequence is moved towards the front of the order, because it is likely to reject the next
 * one too. */
static void test_sequence(int n)
{
    for (int j = 0; j < nvecs; j++) {
        const struct vec *v = &vecs[order[j]];
        if (run_program(seq, n, v->in, goal->arity) != v->out) {
            int t = order[j];
            order[j] = order[j / 2];
            order[j / 2] = t;
            return;
        }
    }
    nfound++;
    fprintf(stderr, "  sequence %d:\n", nfound);
    for (int i = 0; i < n; i++) {
        print_insn(&seq[i]);
    }
    if (nfound == 1) {
        memcpy(first, seq, sizeof(struct insn) * (size_t)n);
        first_len = n;
    }
}

static void synth(int n_insns, int allowed, int ci, int prune);

/* Try one instruction. At a leaf only the output is compared, with no further recursion. */
static void emit(int op, int s1, int s2, int n_insns, int allowed, int ci)
{
    int n_values = goal->arity + n_insns;
    int co;
    word v = exec(op, fetch(values, s1), fetch(values, s2), ci, &co);
    n_nodes++;
    seq[n_insns] = (struct insn){op, s1, s2, n_values};
    if (allowed > 1) {
        values[n_values] = v;
        synth(n_insns + 1, allowed - 1, co, sets_carry(op) ? CY_JUST_SET : 0);
    } else {
        n_leaves++;
        if (v == goal_value) {
            n_candidates++;
            test_sequence(n_insns + 1);
        }
    }
}

/* Whether the prunes drop this instruction. r is its highest operand register, which is the
 * previous instruction's result exactly when the instruction consumes it. */
static int pruned(int op, int leaf, int last_dest, int r, int ci, int prune)
{
    if (reads_carry(op) && ci < 0) {
        return 1;
    }
    if (!leaf || last_dest < 0) {
        return 0;
    }
    if (reads_carry(op) && (prune & CY_JUST_SET)) {
        return 0;
    }
    return r != last_dest;
}

/* Operations on two different registers. A commutative one is tried in one order only. */
static const struct {
    int op, commutative;
} binops[] = {
    {OP_ADD, 1},  {OP_SUB, 0},    {OP_AND, 1},    {OP_IOR, 1},    {OP_XOR, 1},
    {OP_ANDC, 0}, {OP_NOR, 1},    {OP_ADD_CO, 1}, {OP_SUB_CO, 0}, {OP_ADD_CI, 1},
    {OP_SUB_CI, 0},
};

/* Operations on one register, where the other operand is a constant or the same register. GSO
 * lists these by hand, so only the constants that matter are tried, never all of 2^32. */
#define X (-1)
static const struct {
    int op, s1, s2;
} forms[] = {
    {OP_SUB, C0, X},       {OP_SUB, CM1, X},       {OP_LSHIFTR, X, C1},   {OP_LSHIFTR, X, C31},
    {OP_ASHIFTR, X, C31},  {OP_AND, X, C1},        {OP_XOR, X, C1},       {OP_SUB_CO, C0, X},
    {OP_ADD_CI, X, C0},    {OP_ADD_CO, X, X},      {OP_ADD_CI, X, X},     {OP_SUB_CI, X, X}, {OP_ADD_CO, X, CM1},
};

/* Every sequence that extends the n_insns instructions already in seq. */
static void synth(int n_insns, int allowed, int ci, int prune)
{
    int n_values = goal->arity + n_insns;
    int last_dest = n_insns > 0 ? n_values - 1 : -1;
    int leaf = allowed <= 1;

    for (int s1 = n_values - 1; s1 >= 0; s1--) {
        for (int s2 = s1 - 1; s2 >= 0; s2--) {
            for (size_t k = 0; k < sizeof(binops) / sizeof(binops[0]); k++) {
                int op = binops[k].op;
                if (pruned(op, leaf, last_dest, s1, ci, prune)) {
                    continue;
                }
                emit(op, s1, s2, n_insns, allowed, ci);
                if (!binops[k].commutative) {
                    emit(op, s2, s1, n_insns, allowed, ci);
                }
            }
        }
    }
    for (int r = n_values - 1; r >= 0; r--) {
        for (size_t k = 0; k < sizeof(forms) / sizeof(forms[0]); k++) {
            int op = forms[k].op;
            if (pruned(op, leaf, last_dest, r, ci, prune)) {
                continue;
            }
            emit(op, forms[k].s1 == X ? r : forms[k].s1, forms[k].s2 == X ? r : forms[k].s2, n_insns,
                 allowed, ci);
        }
    }
}

/* ------------------------------------------------------------------------------------------
 * Stages
 * ---------------------------------------------------------------------------------------- */

static int stage_goal(const char *name)
{
    for (size_t i = 0; i < sizeof(goals) / sizeof(goals[0]); i++) {
        if (strcmp(goals[i].name, name) == 0) {
            goal = &goals[i];
        }
    }
    if (goal == NULL) {
        return 0;
    }
    build_vectors();
    fprintf(stderr, "== stage_goal: %s, %d input(s), %d test vectors ==\n", goal->name, goal->arity, nvecs);
    fprintf(stderr, "  %s\n", goal->text);
    fprintf(stderr, "  vectors: all combinations of 0, 1, 2, 0x7fffffff, 0x80000000, 0xffffffff, then %d pseudo-random\n",
            N_RANDOM);
    return 1;
}

static void stage_search(void)
{
    fprintf(stderr, "== stage_search: every sequence of length 1, 2, ... until one passes ==\n");
    int pick = 0;
    while (vecs[pick].out == 0) {
        pick++;
    }
    goal_value = vecs[pick].out;
    memcpy(values, vecs[pick].in, sizeof(word) * (size_t)goal->arity);
    fprintf(stderr, "  first test: the vector with output %08x\n", goal_value);
    for (int length = 1; nfound == 0; length++) {
        long nodes0 = n_nodes, leaves0 = n_leaves, cand0 = n_candidates;
        synth(0, length, -1, 0);
        fprintf(stderr, "  length %d: %ld instructions tried, %ld leaves, %ld matched the first test, %d passed all\n", length,
                n_nodes - nodes0, n_leaves - leaves0, n_candidates - cand0, nfound);
    }
}

static int stage_verify(void)
{
    fprintf(stderr, "== stage_verify: the first sequence, on every vector and on a held-out set ==\n");
    for (int i = 0; i < first_len; i++) {
        print_insn(&first[i]);
    }
    for (int k = 0; k < nvecs; k++) {
        for (int i = 0; i < goal->arity; i++) {
            printf("%sin%d=%08x", i == 0 ? "" : " ", i, vecs[k].in[i]);
        }
        printf(" -> %08x\n", run_program(first, first_len, vecs[k].in, goal->arity));
    }
    int bad = 0;
    lcg_state = 2;
    for (int k = 0; k < HELD_OUT; k++) {
        word in[MAX_INPUTS];
        for (int i = 0; i < goal->arity; i++) {
            in[i] = lcg();
        }
        bad += run_program(first, first_len, in, goal->arity) != goal->eval(in);
    }
    fprintf(stderr, "  held-out check: %d random vectors, %d mismatches\n", HELD_OUT, bad);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !stage_goal(argv[1])) {
        fprintf(stderr, "usage: %s <kernel>\n", argv[0]);
        return 2;
    }
    stage_search();
    return stage_verify() != 0;
}
