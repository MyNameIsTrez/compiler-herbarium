/*
 * stoke: a cut-down version of STOKE, the stochastic superoptimizer of Stanford.
 *
 * Like any superoptimizer it is given a function and searches for fast code that computes it,
 * with the function used only as an oracle on test inputs. Where GSO tries every sequence,
 * STOKE does a random walk over whole programs, the Metropolis-Hastings algorithm:
 *   - the state is one complete program, here a fixed number of slots that may hold a nop,
 *   - a move changes it a little: a new opcode, a new operand, a whole new instruction, or two
 *     instructions swapped, each picked with a weight,
 *   - the program is scored by a cost, and a proposal is kept if its cost is at most
 *     `cost - log(p) / beta` for a random p in (0, 1). So it is always kept when it is no
 *     worse, and kept with probability exp(-beta * increase) when it is worse. The walk can
 *     climb out of a local minimum, and the search never needs to know how far the goal is,
 *   - the cost function stops adding up as soon as it passes that bound, so a bad proposal is
 *     rejected after a few test inputs.
 * The cost is the number of wrong bits over all test inputs (a program that is nearly right
 * scores low, which gives the walk a slope to follow), plus, when optimizing, one per
 * instruction. A search that starts from nops and has no instruction cost is "synthesis". One
 * that starts from a correct program and has the instruction cost is "optimization".
 * Nothing is proved: a program is correct when it passes the tests.
 *
 * The toy instruction set is two-address like x86, with a carry flag.
 *
 * Usage: ./a.out <kernel>
 *   Prints, for every test vector, the output of the best correct program, on stdout.
 *   Prints the goal, both searches and the verification on stderr.
 *
 * Stages:
 *   stage_goal    pick the goal function and build the test vectors
 *   stage_search  synthesis from an empty program, then optimization from the result
 *   stage_verify  run the best correct program on every vector, then on a held-out set
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t word;

#define MAX_INPUTS 2
#define NREG 4
#define SLOTS 6
#define MAX_VECS 64
#define HELD_OUT 1000
#define CHAINS 4
#define SYNTH_CAP 400000
#define OPT_ITERATIONS 1500000

/* ------------------------------------------------------------------------------------------
 * The instruction set. Registers r0 and r1 hold the inputs, the others and the carry flag start
 * at zero, and r0 is the output. Each operation has an operand shape, which the moves use to
 * pick valid operands.
 * ---------------------------------------------------------------------------------------- */

enum {
    OP_NOP,
    OP_MOV,
    OP_ADD,
    OP_SUB,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_ADC,
    OP_SBB,
    OP_CMP,
    OP_NEG,
    OP_NOT,
    OP_SHR,
    OP_SAR,
    N_OPS
};

static const char *const op_names[N_OPS] = {"nop", "mov", "add", "sub", "and", "or",  "xor",
                                            "adc", "sbb", "cmp", "neg", "not", "shr", "sar"};

/* The shape of an operation decides which operands it has, and an opcode move only swaps
 * operations of the same shape, so the operands stay valid. */
enum { SHAPE_NONE, SHAPE_BINARY, SHAPE_UNARY, SHAPE_SHIFT, N_SHAPES };

static int shape_of(int op)
{
    if (op == OP_NOP) {
        return SHAPE_NONE;
    }
    if (op == OP_NEG || op == OP_NOT) {
        return SHAPE_UNARY;
    }
    if (op == OP_SHR || op == OP_SAR) {
        return SHAPE_SHIFT;
    }
    return SHAPE_BINARY;
}

/* A source operand is a register number, or NREG plus an index into consts. A shift count may
 * only be the constants 1 and 31. */
enum { C0 = NREG, C1, CM1, C31, N_SRC };
static const word consts[] = {0, 1, 0xffffffffu, 31};

struct insn {
    int op, d, s;
};

static word fetch(const word *regs, int src)
{
    return src < NREG ? regs[src] : consts[src - NREG];
}

/* Run one instruction. The carry flag follows x86: add and adc carry out, sub, sbb, cmp and neg
 * borrow, the logic operations clear it, and a shift leaves the last bit shifted out. */
static void exec(const struct insn *in, word *regs, int *cf)
{
    word a = regs[in->d];
    word b = fetch(regs, in->s);
    word c = (word)*cf;
    switch (in->op) {
    case OP_NOP:
        break;
    case OP_MOV:
        regs[in->d] = b;
        break;
    case OP_ADD:
        regs[in->d] = a + b;
        *cf = regs[in->d] < a;
        break;
    case OP_SUB:
        regs[in->d] = a - b;
        *cf = a < b;
        break;
    case OP_AND:
        regs[in->d] = a & b;
        *cf = 0;
        break;
    case OP_OR:
        regs[in->d] = a | b;
        *cf = 0;
        break;
    case OP_XOR:
        regs[in->d] = a ^ b;
        *cf = 0;
        break;
    case OP_ADC:
        regs[in->d] = a + b + c;
        *cf = (uint64_t)a + b + c > 0xffffffffu;
        break;
    case OP_SBB:
        regs[in->d] = a - b - c;
        *cf = (uint64_t)a < (uint64_t)b + c;
        break;
    case OP_CMP:
        *cf = a < b;
        break;
    case OP_NEG:
        regs[in->d] = 0 - a;
        *cf = a != 0;
        break;
    case OP_NOT:
        regs[in->d] = ~a;
        break;
    case OP_SHR:
        regs[in->d] = a >> b;
        *cf = (a >> (b - 1)) & 1;
        break;
    default: /* OP_SAR */
        regs[in->d] = (word)((int32_t)a >> b);
        *cf = (a >> (b - 1)) & 1;
        break;
    }
}

static word run_program(const struct insn *prog, const word *in, int arity)
{
    word regs[NREG] = {0};
    int cf = 0;
    memcpy(regs, in, sizeof(word) * (size_t)arity);
    for (int i = 0; i < SLOTS; i++) {
        exec(&prog[i], regs, &cf);
    }
    return regs[0];
}

static int length_of(const struct insn *prog)
{
    int n = 0;
    for (int i = 0; i < SLOTS; i++) {
        n += prog[i].op != OP_NOP;
    }
    return n;
}

static void print_src(int src)
{
    if (src < NREG) {
        fprintf(stderr, "r%d", src);
    } else {
        fprintf(stderr, "#%d", (int)consts[src - NREG]);
    }
}

static void print_program(const struct insn *prog)
{
    for (int i = 0; i < SLOTS; i++) {
        if (prog[i].op == OP_NOP) {
            continue;
        }
        int shape = shape_of(prog[i].op);
        fprintf(stderr, "    %s r%d", op_names[prog[i].op], prog[i].d);
        if (shape == SHAPE_BINARY || shape == SHAPE_SHIFT) {
            fprintf(stderr, ", ");
            print_src(prog[i].s);
        }
        fprintf(stderr, "\n");
    }
}

/* ------------------------------------------------------------------------------------------
 * Goal functions. Test vectors: every combination of a few edge values, then pseudo-random
 * words, the same rule for every entry that runs on kernels/.
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
    }
}

/* ------------------------------------------------------------------------------------------
 * The cost function. As in STOKE it is told how much it may add up to (`cutoff`) and stops as
 * soon as the sum reaches it, because the walk is going to reject the proposal anyway.
 * ---------------------------------------------------------------------------------------- */

static long n_evals, n_cut;

static long cost(const struct insn *prog, long cutoff, long insn_weight, int *correct)
{
    long perf = insn_weight * length_of(prog);
    long c = perf;
    int k = 0;
    n_evals++;
    while (c < cutoff && k < nvecs) {
        c += __builtin_popcount(run_program(prog, vecs[k].in, goal->arity) ^ vecs[k].out);
        k++;
    }
    n_cut += k < nvecs;
    *correct = c == perf;
    return c;
}

/* ------------------------------------------------------------------------------------------
 * The moves. Each one returns 0 when the proposal is not valid, and otherwise leaves what it
 * needs to undo itself in struct undo.
 * ---------------------------------------------------------------------------------------- */

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static word rnd(word n)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (word)((rng_state * 0x2545f4914f6cdd1dull) >> 33) % n;
}

enum { MOVE_INSTRUCTION, MOVE_OPCODE, MOVE_OPERAND, MOVE_SWAP, N_MOVES };
static const char *const move_names[N_MOVES] = {"instruction", "opcode", "operand", "swap"};
/* STOKE gives each move a mass and picks from the sum. Its local swap and global swap are the
 * same move here, because a program is one block, so swap has both of their masses. */
static const int move_mass[N_MOVES] = {1, 1, 1, 2};

struct undo {
    int i, j;
    struct insn old_i, old_j;
};

static int random_src(int shape)
{
    if (shape == SHAPE_SHIFT) {
        return rnd(2) ? C31 : C1;
    }
    return (int)rnd(N_SRC);
}

static int random_op(void)
{
    return (int)rnd(N_OPS);
}

static int random_op_of_shape(int shape)
{
    int op = random_op();
    while (shape_of(op) != shape) {
        op = random_op();
    }
    return op;
}

static int move_instruction(struct insn *prog, struct undo *u)
{
    u->i = (int)rnd(SLOTS);
    u->old_i = prog[u->i];
    struct insn n = {random_op(), (int)rnd(NREG), 0};
    n.s = random_src(shape_of(n.op));
    prog[u->i] = n;
    return 1;
}

static int move_opcode(struct insn *prog, struct undo *u)
{
    u->i = (int)rnd(SLOTS);
    u->old_i = prog[u->i];
    prog[u->i].op = random_op_of_shape(shape_of(prog[u->i].op));
    return 1;
}

static int move_operand(struct insn *prog, struct undo *u)
{
    u->i = (int)rnd(SLOTS);
    u->old_i = prog[u->i];
    int shape = shape_of(prog[u->i].op);
    if (shape == SHAPE_NONE) {
        return 0;
    }
    if (shape == SHAPE_UNARY || rnd(2)) {
        prog[u->i].d = (int)rnd(NREG);
    } else {
        prog[u->i].s = random_src(shape);
    }
    return 1;
}

static int move_swap(struct insn *prog, struct undo *u)
{
    u->i = (int)rnd(SLOTS);
    u->j = (int)rnd(SLOTS);
    if (u->i == u->j) {
        return 0;
    }
    u->old_i = prog[u->i];
    u->old_j = prog[u->j];
    prog[u->i] = u->old_j;
    prog[u->j] = u->old_i;
    return 1;
}

static int propose(struct insn *prog, struct undo *u, int *move)
{
    int total = 0;
    for (int m = 0; m < N_MOVES; m++) {
        total += move_mass[m];
    }
    int pick = (int)rnd((word)total);
    *move = 0;
    while (pick >= move_mass[*move]) {
        pick -= move_mass[*move];
        (*move)++;
    }
    switch (*move) {
    case MOVE_INSTRUCTION:
        return move_instruction(prog, u);
    case MOVE_OPCODE:
        return move_opcode(prog, u);
    case MOVE_OPERAND:
        return move_operand(prog, u);
    default: /* MOVE_SWAP */
        return move_swap(prog, u);
    }
}

static void undo_move(struct insn *prog, const struct undo *u, int move)
{
    prog[u->i] = u->old_i;
    if (move == MOVE_SWAP) {
        prog[u->j] = u->old_j;
    }
}

/* ------------------------------------------------------------------------------------------
 * The search: STOKE's Search::run. `thr[d]` is 2^32 * exp(-d), so a random p below it means
 * the walk accepts a proposal that is d worse. STOKE computes `max = cost - log(p) / beta`
 * with beta 1, which is the same bound, without needing a logarithm.
 * ---------------------------------------------------------------------------------------- */

#define MAX_DELTA 24

static uint64_t thr[MAX_DELTA + 1];

static void init_thresholds(void)
{
    thr[0] = 1ull << 32;
    for (int d = 1; d <= MAX_DELTA; d++) {
        thr[d] = thr[d - 1] * 1580030169ull >> 32; /* exp(-1) in 0.32 fixed point */
    }
}

/* The most that a proposal may be worse than the current program, for this p. */
static long allowed_increase(word p)
{
    long d = 0;
    while (p < thr[d + 1]) { /* the table ends in zeros, which no p is below */
        d++;
    }
    return d;
}

struct search_state {
    struct insn current[SLOTS], best_yet[SLOTS], best_correct[SLOTS];
    long current_cost, best_yet_cost, best_correct_cost;
    int success;
};

static struct search_state state;
static struct insn best[SLOTS];
static int have_best;
static long stat_proposed[N_MOVES], stat_succeeded[N_MOVES], stat_accepted[N_MOVES];

static void run_search(const char *name, long insn_weight, long max_iterations)
{
    int correct;
    memset(stat_proposed, 0, sizeof(stat_proposed));
    memset(stat_succeeded, 0, sizeof(stat_succeeded));
    memset(stat_accepted, 0, sizeof(stat_accepted));
    n_evals = n_cut = 0;
    state.current_cost = cost(state.current, 1L << 40, insn_weight, &correct);
    memcpy(state.best_yet, state.current, sizeof(state.current));
    state.best_yet_cost = state.current_cost;
    state.success = correct;
    memcpy(state.best_correct, state.current, sizeof(state.current));
    state.best_correct_cost = correct ? state.current_cost : 1L << 40;
    fprintf(stderr, "  %s: start at cost %ld\n", name, state.current_cost);

    long iterations = 0;
    for (; state.current_cost > 0 && iterations < max_iterations; iterations++) {
        struct undo u;
        int move;
        if (!propose(state.current, &u, &move)) {
            stat_proposed[move]++;
            continue;
        }
        stat_proposed[move]++;
        stat_succeeded[move]++;

        long max = state.current_cost + allowed_increase(rnd(0xffffffffu));
        long new_cost = cost(state.current, max + 1, insn_weight, &correct);
        if (new_cost > max) {
            undo_move(state.current, &u, move);
            continue;
        }
        stat_accepted[move]++;
        state.current_cost = new_cost;
        if (new_cost < state.best_yet_cost) {
            memcpy(state.best_yet, state.current, sizeof(state.current));
            state.best_yet_cost = new_cost;
        }
        if (correct && new_cost < state.best_correct_cost) {
            memcpy(state.best_correct, state.current, sizeof(state.current));
            state.best_correct_cost = new_cost;
            state.success = 1;
            fprintf(stderr, "    iteration %ld: correct with %d instructions\n", iterations, length_of(state.current));
        }
    }
    fprintf(stderr, "  %s: %ld iterations, %ld cost evaluations, %ld cut short, lowest cost %ld\n", name, iterations, n_evals,
            n_cut, state.best_yet_cost);
    for (int m = 0; m < N_MOVES; m++) {
        fprintf(stderr, "    %-11s proposed %8ld, valid %8ld, accepted %8ld\n", move_names[m], stat_proposed[m],
                stat_succeeded[m], stat_accepted[m]);
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

/* One chain can get stuck in a local minimum. The real `search` tool restarts from a fresh state
 * when a chain times out without a verified result, and the paper runs many chains in parallel
 * and returns the best. Here a chain that does not reach a correct program within the cap is
 * thrown away and a new one is started from nops. Each chain that reaches one then optimizes
 * it, and the shortest correct program of all the chains is kept. */
static void reseed(long n)
{
    rng_state = 0x9e3779b97f4a7c15ull + (uint64_t)n * 0xd1b54a32d192ed03ull;
}

static void stage_search(void)
{
    fprintf(stderr, "== stage_search: %d chains, each synthesis from %d nops, then optimization ==\n", CHAINS, SLOTS);
    init_thresholds();
    long attempt = 0;
    for (int chain = 0; chain < CHAINS; chain++) {
        do {
            attempt++;
            reseed(attempt);
            memset(state.current, 0, sizeof(state.current));
            fprintf(stderr, "  chain %d, attempt %ld\n", chain + 1, attempt);
            run_search("synthesis (cost = wrong bits)", 0, SYNTH_CAP);
        } while (!state.success);
        run_search("optimization (cost = wrong bits + instructions)", 1, OPT_ITERATIONS);
        if (!have_best || length_of(state.best_correct) < length_of(best)) {
            memcpy(best, state.best_correct, sizeof(best));
            have_best = 1;
        }
    }
}

static int stage_verify(void)
{
    fprintf(stderr, "== stage_verify: the best correct program, on every vector and on a held-out set ==\n");
    fprintf(stderr, "  %d instructions\n", length_of(best));
    print_program(best);
    for (int k = 0; k < nvecs; k++) {
        for (int i = 0; i < goal->arity; i++) {
            printf("%sin%d=%08x", i == 0 ? "" : " ", i, vecs[k].in[i]);
        }
        printf(" -> %08x\n", run_program(best, vecs[k].in, goal->arity));
    }
    int bad = 0;
    lcg_state = 2;
    for (int k = 0; k < HELD_OUT; k++) {
        word in[MAX_INPUTS];
        for (int i = 0; i < goal->arity; i++) {
            in[i] = lcg();
        }
        bad += run_program(best, in, goal->arity) != goal->eval(in);
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
