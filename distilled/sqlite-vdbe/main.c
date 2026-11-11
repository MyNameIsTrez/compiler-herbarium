/*
 * sqlite-vdbe: a cut-down version of the query planner and virtual machine of SQLite.
 *
 * SQL says what rows to return, not how to find them. SQLite turns a SELECT into a nest of
 * loops, one per table of the FROM clause, and the interesting decision is which loop each
 * table gets and in what order the loops nest. For every table the planner lists the ways to
 * walk it (a full scan, a lookup by rowid, an index seek on an equality or a range), and gives
 * each way a cost in LogEst, a logarithmic unit where adding 10 doubles the number. Then a
 * dynamic program over the tables keeps the cheapest nesting. The chosen loops are compiled to
 * a register bytecode that a cursor-based virtual machine (the VDBE) runs: open a b-tree,
 * seek, step, read a column, jump out of the loop, return a row.
 *
 * The SQL parser and name resolution are not here: a table of examples holds each query as the
 * planner receives it (tables, columns, WHERE terms), next to the SQL it came from. There is no
 * b-tree either: a table is an array of rows sorted by rowid and an index is an array of
 * (key, rowid) pairs sorted by key. Terms on the id column are equalities only.
 *
 * Usage: ./a.out <example> [code.bin]
 *   Prints the rows of the query on stdout, as `sqlite3` does, and the intermediate form after
 *   each stage on stderr. The bytecode is run by an interpreter, so the code.bin argument is
 *   ignored.
 *
 * Stages:
 *   stage_schema   the catalog: default row estimates and row widths in LogEst, and the index
 *                  entries sorted by key
 *   stage_query    the query, its WHERE terms, and which columns of each table it reads
 *   stage_loops    every WhereLoop (one way to walk one table) with its cost, keeping only the
 *                  ones that no other loop beats
 *   stage_solve    the path solver: the cheapest nesting of the loops, one table per round
 *   stage_codegen  the nested loops as bytecode, then the reads redirected to the index
 *   stage_run      run the bytecode, showing each instruction as it executes
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define MAX_FROM 2
#define MAX_COLS 3
#define MAX_ROWS 8
#define MAX_TERMS 4
#define MAX_SEL 3
#define MAX_LOOPS 32
#define MAX_PROG 96
#define MAX_REGS 16
#define MAX_CURS 4
#define SCHEMA_COOKIE 4

/* A cost is a LogEst: 10 * log2(the number). 200 is about a million and 33 is about ten. */
typedef int logest;

struct value {
    bool text;
    long num;
    const char *str;
};

struct row {
    long rowid;
    struct value v[MAX_COLS];
};

/* Column 0 of every table is its INTEGER PRIMARY KEY, which is the rowid. */
struct table {
    const char *name;
    int root;
    int ncol;
    const char *col[MAX_COLS];
    int nrow;
    struct row row[MAX_ROWS];
    logest nrowest;
    logest szrow;
};

struct entry {
    struct value key;
    long rowid;
};

/* An index has one key column and the rowid after it. */
struct index {
    const char *name;
    int table;
    int col;
    int root;
    int n;
    struct entry e[MAX_ROWS];
    logest rowest[2];
    logest szidx;
};

enum { T_USERS, T_ORDERS, N_TABLES };
enum { I_ORDERS_USER, I_USERS_AGE, N_INDEXES };

#define T(s) { .text = true, .str = s }
#define N(n) { .num = n }

static struct table tables[N_TABLES] = {
    [T_USERS] = { .name = "users", .root = 2, .ncol = 3, .col = { "id", "name", "age" }, .nrow = 3,
                  .row = { { 1, { N(1), T("ann"), N(30) } },
                           { 2, { N(2), T("bob"), N(25) } },
                           { 3, { N(3), T("cy"), N(30) } } } },
    [T_ORDERS] = { .name = "orders", .root = 3, .ncol = 3, .col = { "id", "user", "amount" },
                   .nrow = 4,
                   .row = { { 1, { N(1), N(1), N(10) } },
                            { 2, { N(2), N(1), N(20) } },
                            { 3, { N(3), N(3), N(5) } },
                            { 4, { N(4), N(2), N(7) } } } },
};

static struct index indexes[N_INDEXES] = {
    [I_ORDERS_USER] = { .name = "orders_user", .table = T_ORDERS, .col = 1, .root = 4 },
    [I_USERS_AGE] = { .name = "users_age", .table = T_USERS, .col = 2, .root = 5 },
};

/* ---- LogEst arithmetic, as in util.c ---- */

/* x is at least 8 here: every caller passes a width times 4 or a row estimate above 10. */
static logest log_est(unsigned long long x)
{
    static const logest a[] = { 0, 2, 3, 5, 6, 7, 8, 9 };
    int i = 60 - __builtin_clzll(x);
    return a[(x >> i) & 7] + 40 + i * 10 - 10;
}

/* log2 of the number of rows, in LogEst, with 33 taken off. SQLite returns 0 for 10 rows or
 * fewer, but the row estimate of a table here is always 200. */
static logest est_log(logest n)
{
    return log_est((unsigned long long)n) - 33;
}

/* The LogEst of the sum of two numbers whose LogEst are a and b. */
static logest log_est_add(logest a, logest b)
{
    static const unsigned char x[] = { 10, 10, 9, 9, 8, 8, 7, 7, 7, 6, 6, 6, 5, 5, 5, 4,
                                       4, 4, 4, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2 };
    if (a < b) {
        logest t = a;
        a = b;
        b = t;
    }
    if (a > b + 49) {
        return a;
    }
    if (a > b + 31) {
        return a + 1;
    }
    return a + x[a - b];
}

/* ---- the query ---- */

enum { EQ, GT, GE, LT, LE };
static const char *const op_text[] = { "=", ">", ">=", "<", "<=" };
static const int op_flip[] = { EQ, LT, LE, GT, GE };

/* tab is a position in the FROM clause, or -1 for a constant. */
struct operand {
    int tab;
    int col;
    struct value k;
};

struct term {
    struct operand l, r;
    int op;
};

struct query {
    const char *sql;
    int nfrom;
    int from[MAX_FROM];
    int nsel;
    struct operand sel[MAX_SEL];
    int nterm;
    struct term term[MAX_TERMS];
};

#define COL(t, c) { .tab = t, .col = c }
#define INT(n) { .tab = -1, .k = { .num = n } }
#define STR(s) { .tab = -1, .k = { .text = true, .str = s } }

static const struct { const char *name; struct query q; } examples[] = {
    { "scan", { "SELECT id, name FROM users WHERE name = 'cy'",
                1, { T_USERS }, 2, { COL(0, 0), COL(0, 1) },
                1, { { COL(0, 1), STR("cy"), EQ } } } },
    { "rowid", { "SELECT name FROM users WHERE id = 3",
                 1, { T_USERS }, 1, { COL(0, 1) },
                 1, { { COL(0, 0), INT(3), EQ } } } },
    { "index", { "SELECT name FROM users WHERE age = 30",
                 1, { T_USERS }, 1, { COL(0, 1) },
                 1, { { COL(0, 2), INT(30), EQ } } } },
    { "cover", { "SELECT id FROM users WHERE age = 30",
                 1, { T_USERS }, 1, { COL(0, 0) },
                 1, { { COL(0, 2), INT(30), EQ } } } },
    { "range", { "SELECT name FROM users WHERE age > 26",
                 1, { T_USERS }, 1, { COL(0, 1) },
                 1, { { COL(0, 2), INT(26), GT } } } },
    { "between", { "SELECT name FROM users WHERE age >= 25 AND age < 30",
                   1, { T_USERS }, 1, { COL(0, 1) },
                   2, { { COL(0, 2), INT(25), GE }, { COL(0, 2), INT(30), LT } } } },
    { "between-gt", { "SELECT name FROM users WHERE age > 25 AND age <= 30",
                      1, { T_USERS }, 1, { COL(0, 1) },
                      2, { { COL(0, 2), INT(25), GT }, { COL(0, 2), INT(30), LE } } } },
    { "join-rowid", { "SELECT users.name, orders.id FROM users, orders "
                      "WHERE orders.user = users.id AND users.age = 25",
                      2, { T_USERS, T_ORDERS }, 2, { COL(0, 1), COL(1, 0) },
                      2, { { COL(1, 1), COL(0, 0), EQ }, { COL(0, 2), INT(25), EQ } } } },
    { "below", { "SELECT name FROM users WHERE age <= 25",
                 1, { T_USERS }, 1, { COL(0, 1) },
                 1, { { COL(0, 2), INT(25), LE } } } },
    { "cover-scan", { "SELECT age FROM users", .nfrom = 1, .from = { T_USERS }, .nsel = 1,
                      .sel = { COL(0, 2) } } },
    { "residual", { "SELECT amount FROM orders WHERE amount > 6 AND user = 1",
                    1, { T_ORDERS }, 1, { COL(0, 2) },
                    2, { { COL(0, 2), INT(6), GT }, { COL(0, 1), INT(1), EQ } } } },
    { "index-miss", { "SELECT name FROM users WHERE age = 99",
                      1, { T_USERS }, 1, { COL(0, 1) },
                      1, { { COL(0, 2), INT(99), EQ } } } },
    { "rowid-miss", { "SELECT name FROM users WHERE id = 9",
                      1, { T_USERS }, 1, { COL(0, 1) },
                      1, { { COL(0, 0), INT(9), EQ } } } },
    { "residual-ops", { "SELECT amount FROM orders "
                        "WHERE user = 1 AND amount >= 10 AND amount < 20 AND amount <= 15",
                        1, { T_ORDERS }, 1, { COL(0, 2) },
                        4, { { COL(0, 1), INT(1), EQ }, { COL(0, 2), INT(10), GE },
                             { COL(0, 2), INT(20), LT }, { COL(0, 2), INT(15), LE } } } },
    { "join", { "SELECT users.name, orders.amount FROM users, orders WHERE orders.user = users.id",
                2, { T_USERS, T_ORDERS }, 2, { COL(0, 1), COL(1, 2) },
                1, { { COL(1, 1), COL(0, 0), EQ } } } },
    { "join-index", { "SELECT users.name, orders.amount FROM users, orders "
                      "WHERE orders.user = users.id AND users.age = 25",
                      2, { T_USERS, T_ORDERS }, 2, { COL(0, 1), COL(1, 2) },
                      2, { { COL(1, 1), COL(0, 0), EQ }, { COL(0, 2), INT(25), EQ } } } },
    { "join-filter", { "SELECT users.name FROM users, orders "
                       "WHERE orders.user = users.id AND orders.amount > 6",
                       2, { T_USERS, T_ORDERS }, 1, { COL(0, 1) },
                       2, { { COL(1, 1), COL(0, 0), EQ }, { COL(1, 2), INT(6), GT } } } },
};

static struct query q;

static const char *tname(int f)
{
    return tables[q.from[f]].name;
}

static const char *cname(int f, int c)
{
    return tables[q.from[f]].col[c];
}

static void print_value(FILE *out, const struct value *v)
{
    if (v->text) {
        fputs(v->str, out);
    } else {
        fprintf(out, "%ld", v->num);
    }
}

static void print_operand(FILE *out, const struct operand *o)
{
    if (o->tab < 0) {
        if (o->k.text) {
            fprintf(out, "'%s'", o->k.str);
        } else {
            print_value(out, &o->k);
        }
    } else {
        fprintf(out, "%s.%s", tname(o->tab), cname(o->tab, o->col));
    }
}

static int cmp_value(const struct value *a, const struct value *b)
{
    if (a->text) {
        return strcmp(a->str, b->str);
    }
    return (a->num > b->num) - (a->num < b->num);
}

/* A WHERE term seen from one of its two columns: "orders.user = users.id" can drive a loop over
 * orders (user = ?) or a loop over users (id = ?). */
struct cons {
    int term;
    bool flip;
};

static struct cons cons[2 * MAX_TERMS];
static int ncons;
static unsigned col_used[MAX_FROM];

static const struct operand *cons_lhs(const struct cons *c)
{
    return c->flip ? &q.term[c->term].r : &q.term[c->term].l;
}

static const struct operand *cons_rhs(const struct cons *c)
{
    return c->flip ? &q.term[c->term].l : &q.term[c->term].r;
}

static int cons_op(const struct cons *c)
{
    return c->flip ? op_flip[q.term[c->term].op] : q.term[c->term].op;
}

/* The tables a term mentions, as a bitmask over the FROM clause. */
static unsigned term_mask(const struct term *t)
{
    return (1u << t->l.tab) | (t->r.tab >= 0 ? 1u << t->r.tab : 0);
}

static void use_column(const struct operand *o)
{
    if (o->tab >= 0) {
        col_used[o->tab] |= 1u << o->col;
    }
}

/* ---- the planner ---- */

#define W_IPK 0x01        /* walks the table by rowid */
#define W_INDEXED 0x02    /* walks an index */
#define W_IDX_ONLY 0x04   /* the index holds every column the query reads */
#define W_EQ 0x08         /* an equality on the key */
#define W_BTM 0x10        /* a lower bound on the key */
#define W_TOP 0x20        /* an upper bound on the key */
#define W_ONEROW 0x40     /* the equality finds at most one row */
#define W_UNQ_WANTED 0x80 /* a unique index could find just one row, this one cannot */
#define W_CONSTRAINT (W_EQ | W_BTM | W_TOP)

struct loop {
    int from;      /* position in the FROM clause */
    int idx;       /* index id, or -1 for the rowid */
    unsigned flags;
    int nl;
    int lterm[2];  /* indexes into cons[] */
    logest rrun;   /* cost of one run of the loop */
    logest nout;   /* rows one run yields */
    unsigned prereq; /* the tables that must be outer to this loop */
    unsigned self;
};

static struct loop loops[MAX_LOOPS];
static unsigned nloops;

static int index_of(int table)
{
    return table == T_USERS ? I_USERS_AGE : I_ORDERS_USER;
}

static void describe(const struct loop *lp, char *buf, size_t size)
{
    const char *tn = tname(lp->from);
    int n = snprintf(buf, size, "%s %s", lp->flags & W_CONSTRAINT ? "SEARCH" : "SCAN", tn);
    if (lp->idx >= 0) {
        n += snprintf(buf + n, size - (size_t)n, " USING %sINDEX %s",
                      lp->flags & W_IDX_ONLY ? "COVERING " : "", indexes[lp->idx].name);
    } else if (lp->flags & W_CONSTRAINT) {
        n += snprintf(buf + n, size - (size_t)n, " USING INTEGER PRIMARY KEY");
    }
    if (lp->flags & W_CONSTRAINT) {
        const char *col = lp->idx >= 0 ? indexes[lp->idx].name : "rowid";
        if (lp->idx >= 0) {
            col = cname(lp->from, indexes[lp->idx].col);
        }
        n += snprintf(buf + n, size - (size_t)n, " (");
        for (int i = 0; i < lp->nl; i++) {
            int op = cons_op(&cons[lp->lterm[i]]);
            n += snprintf(buf + n, size - (size_t)n, "%s%s%s?", i ? " AND " : "", col,
                          op == EQ ? "=" : op == LT || op == LE ? "<" : ">");
        }
        snprintf(buf + n, size - (size_t)n, ")");
    }
}

static void print_loop(const char *tag, const struct loop *lp)
{
    char buf[128];
    describe(lp, buf, sizeof buf);
    fprintf(stderr, "  %-8s %-62s rRun=%-3d nOut=%-3d prereq=%u\n", tag, buf, lp->rrun, lp->nout,
            lp->prereq);
}

/* Terms of the WHERE clause that the loop does not use but that only need tables the loop can
 * see make the loop yield fewer rows. */
static void output_adjust(struct loop *lp, logest nrow)
{
    logest reduce = 0;
    if (q.nfrom <= 1) {
        return;
    }
    for (int i = 0; i < q.nterm; i++) {
        unsigned all = term_mask(&q.term[i]);
        bool used = false;
        if ((all & ~(lp->prereq | lp->self)) != 0 || (all & lp->self) == 0) {
            continue;
        }
        for (int j = 0; j < lp->nl; j++) {
            used |= cons[lp->lterm[j]].term == i;
        }
        if (used) {
            continue;
        }
        lp->nout--;
        if (q.term[i].op == EQ) {
            reduce = 20;
        }
    }
    if (lp->nout > nrow - reduce) {
        lp->nout = nrow - reduce;
    }
}

#define DISCARD (~0u)

/* Loop a needs no more outer tables than loop b, and costs no more and yields no more. */
static bool no_worse(const struct loop *a, const struct loop *b)
{
    bool prereq_subset = (a->prereq & b->prereq) == a->prereq;
    return prereq_subset & (a->rrun <= b->rrun) & (a->nout <= b->nout);
}

/* The first loop that the template makes redundant, nloops if there is none, or DISCARD if an
 * existing loop is at least as good in every way. */
static unsigned find_lesser(unsigned start, const struct loop *t)
{
    for (unsigned i = start; i < nloops; i++) {
        const struct loop *p = &loops[i];
        if (p->from != t->from) {
            continue;
        }
        if (no_worse(p, t)) {
            return DISCARD;
        }
        if (no_worse(t, p)) {
            return i;
        }
    }
    return nloops;
}

static void loop_insert(struct loop *t)
{
    unsigned at = find_lesser(0, t);
    if (at == DISCARD) {
        print_loop("skip", t);
        return;
    }
    if (at == nloops) {
        print_loop("add", t);
        loops[nloops++] = *t;
        return;
    }
    print_loop("replace", &loops[at]);
    print_loop("with", t);
    loops[at] = *t;
}

/* Cost a loop that seeks the key of a probe: rowid (idx -1) or an index. The estimate of the
 * rows it yields (nout) is already set. */
static void insert_seek(struct loop *lp, logest rsize, logest szidx, logest sztab)
{
    logest logsize = est_log(rsize);
    logest cost = lp->idx < 0 ? lp->nout + 16 : lp->nout + 1 + (15 * szidx) / sztab;
    cost = log_est_add(logsize, cost);
    lp->rrun = cost;
    if (!(lp->flags & (W_IDX_ONLY | W_IPK))) {
        lp->rrun = log_est_add(lp->rrun, lp->nout + 16);
    }
    output_adjust(lp, rsize);
    loop_insert(lp);
}

/* One lower and one upper bound lower the rows of a range scan by 20 each, and by 20 more if both
 * are there. SQLite also keeps the estimate above 10 and below the rows of an equality, which
 * 200 rows never reach. */
static logest range_nout(logest nout, bool lower, bool upper)
{
    return nout - 20 * (lower + upper) - (lower && upper ? 20 : 0);
}

/* Add the loops that use constraints on the key column of one probe. With a lower bound given,
 * look for an upper bound to go with it. */
static void add_seek_loops(const struct loop *base, logest rsize, logest szidx, logest sztab,
                           const logest *rowest, int btm)
{
    for (int c = 0; c < ncons; c++) {
        const struct operand *l = cons_lhs(&cons[c]);
        const struct operand *r = cons_rhs(&cons[c]);
        int op = cons_op(&cons[c]);
        struct loop lp = *base;
        if (l->tab != base->from || l->col != (base->idx < 0 ? 0 : indexes[base->idx].col)) {
            continue;
        }
        if (btm >= 0 && op != LT && op != LE) {
            continue;
        }
        lp.lterm[lp.nl++] = c;
        lp.prereq = r->tab >= 0 ? 1u << r->tab : 0;
        if (op == EQ) {
            lp.flags |= W_EQ | (base->idx < 0 ? W_ONEROW : W_UNQ_WANTED);
            lp.nout = rsize + (rowest[1] - rowest[0]);
        } else {
            bool lower = op == GT || op == GE;
            if (btm >= 0) {
                lp.lterm[0] = btm;
                lp.lterm[1] = c;
                lp.nl = 2;
                lp.flags |= W_BTM | W_TOP;
            } else {
                lp.flags |= lower ? W_BTM : W_TOP;
            }
            lp.nout = range_nout(rsize, lower || btm >= 0, !lower);
        }
        insert_seek(&lp, rsize, szidx, sztab);
        if ((lp.flags & W_BTM) && !(lp.flags & W_TOP)) {
            add_seek_loops(base, rsize, szidx, sztab, rowest, c);
        }
    }
}

static void stage_loops(void)
{
    fprintf(stderr, "== stage_loops: each WhereLoop as it is offered (rRun and nOut in LogEst) ==\n");
    for (int f = 0; f < q.nfrom; f++) {
        const struct table *tb = &tables[q.from[f]];
        const struct index *ix = &indexes[index_of(q.from[f])];
        int id = index_of(q.from[f]);
        logest ipk_rowest[2] = { tb->nrowest, 0 };
        struct loop lp = { .from = f, .self = 1u << f };
        bool covering = (col_used[f] & ~(1u << ix->col | 1u)) == 0;

        /* the rowid: a full scan, then its seeks */
        lp.idx = -1;
        lp.flags = W_IPK;
        lp.nout = tb->nrowest;
        lp.rrun = tb->nrowest + 16;
        output_adjust(&lp, tb->nrowest);
        loop_insert(&lp);
        add_seek_loops(&lp, tb->nrowest, 3, tb->szrow, ipk_rowest, -1);

        /* the index: a full scan of a covering index is cheaper than the table, then its seeks */
        lp.idx = id;
        lp.flags = W_INDEXED | (covering ? W_IDX_ONLY : 0);
        lp.nout = ix->rowest[0];
        if (covering) {
            lp.rrun = ix->rowest[0] + 1 + (15 * ix->szidx) / tb->szrow;
            output_adjust(&lp, ix->rowest[0]);
            loop_insert(&lp);
        }
        add_seek_loops(&lp, ix->rowest[0], ix->szidx, tb->szrow, ix->rowest, -1);
    }
    fprintf(stderr, "kept:\n");
    for (unsigned i = 0; i < nloops; i++) {
        char tag[16];
        snprintf(tag, sizeof tag, "L%u", i);
        print_loop(tag, &loops[i]);
    }
}

/* ---- the path solver ---- */

struct path {
    int loop[MAX_FROM];
    unsigned mask;
    logest nrow, rcost, runsort;
};

struct level {
    int loop;
    int tabcur;
    int idxcur;
    int top; /* the address a Next goes back to */
};

static struct level lev[MAX_FROM];

static void print_path(const struct path *p, int n)
{
    fprintf(stderr, "  ");
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "%sL%d", i ? " -> " : "", p->loop[i]);
    }
    fprintf(stderr, "  cost=%d nrow=%d\n", p->rcost, p->nrow);
}

/* One round per table: extend each path of the last round by a loop for a table it does not
 * have yet, and keep the cheapest path for each set of tables. SQLite also keeps a few more per
 * round when a sort order could make a dearer path pay off, which needs an ORDER BY. */
static void stage_solve(void)
{
    struct path a[1 << MAX_FROM] = { { .mask = 0 } }, b[1 << MAX_FROM];
    struct path *from = a, *to = b;
    int nfrom = 1;
    fprintf(stderr, "== stage_solve: the cheapest path for each set of tables ==\n");
    for (int round = 0; round < q.nfrom; round++) {
        int nto = 0;
        for (int i = 0; i < nfrom; i++) {
            for (unsigned l = 0; l < nloops; l++) {
                const struct loop *lp = &loops[l];
                logest cost = log_est_add(lp->rrun + from[i].nrow, from[i].runsort);
                unsigned mask = from[i].mask | lp->self;
                int j = 0;
                if ((lp->prereq & ~from[i].mask) != 0 || (lp->self & from[i].mask) != 0) {
                    continue;
                }
                while (j < nto && to[j].mask != mask) {
                    j++;
                }
                if (j < nto && cost >= to[j].rcost) {
                    continue;
                }
                nto += j == nto;
                memcpy(to[j].loop, from[i].loop, sizeof(int) * (size_t)round);
                to[j].loop[round] = (int)l;
                to[j].mask = mask;
                to[j].nrow = from[i].nrow + lp->nout;
                to[j].rcost = cost;
                to[j].runsort = cost - 2;
            }
        }
        fprintf(stderr, "round %d:\n", round);
        for (int i = 0; i < nto; i++) {
            print_path(&to[i], round + 1);
        }
        struct path *t = from;
        from = to;
        to = t;
        nfrom = nto;
    }
    fprintf(stderr, "plan:\n");
    for (int i = 0; i < q.nfrom; i++) {
        char buf[128];
        lev[i].loop = from[0].loop[i];
        lev[i].tabcur = loops[lev[i].loop].from;
        describe(&loops[lev[i].loop], buf, sizeof buf);
        fprintf(stderr, "  %s\n", buf);
    }
}

/* ---- the bytecode ---- */

enum {
    OP_INIT, OP_OPENREAD, OP_REWIND, OP_NEXT, OP_SEEKROWID, OP_SEEKGE, OP_SEEKGT, OP_IDXGE,
    OP_IDXGT, OP_DEFERREDSEEK, OP_IDXROWID, OP_COLUMN, OP_ROWID, OP_INTEGER, OP_STRING8,
    OP_NE, OP_LE, OP_LT, OP_GE, OP_GT, OP_RESULTROW, OP_HALT, OP_TRANSACTION, OP_GOTO
};

static const char *const op_name[] = {
    "Init", "OpenRead", "Rewind", "Next", "SeekRowid", "SeekGE", "SeekGT", "IdxGE", "IdxGT",
    "DeferredSeek", "IdxRowid", "Column", "Rowid", "Integer", "String8", "Ne", "Le", "Lt", "Ge",
    "Gt", "ResultRow", "Halt", "Transaction", "Goto"
};

/* The jump that skips a row for which the term is false, indexed by the term's operator. */
static const int skip_op[] = { OP_NE, OP_LE, OP_LT, OP_GE, OP_GT };

struct insn {
    int op, p1, p2, p3, p5;
    char p4[16];
};

static struct insn prog[MAX_PROG];
static int nprog;
static int nmem;

/* A jump target that is not known yet is the negative of a label number. Level i has two labels:
 * the break label (the address after the level's loop) and the continue label (the address of
 * the level's Next, where a row that fails a term goes). */
#define LABEL_INIT 1
#define LABEL_BRK(i) (2 + 2 * (i))
#define LABEL_CONT(i) (3 + 2 * (i))
static int label_addr[4 + 2 * MAX_FROM];

static int emit(int op, int p1, int p2, int p3)
{
    prog[nprog] = (struct insn){ .op = op, .p1 = p1, .p2 = p2, .p3 = p3 };
    return nprog++;
}

static void place(int label)
{
    label_addr[label] = nprog;
}

struct hoisted {
    int reg;
    struct value k;
};

static struct hoisted init_consts[MAX_TERMS];
static int ninit;

static void emit_const(int reg, const struct value *k)
{
    int a = emit(k->text ? OP_STRING8 : OP_INTEGER, k->text ? 0 : (int)k->num, reg, 0);
    if (k->text) {
        snprintf(prog[a].p4, sizeof prog[a].p4, "%s", k->str);
    }
}

/* Put an operand into a register. The constant of a seek key is loaded where it is used, but the
 * constant of a comparison is loaded once by a block after the program, which Init jumps to. */
static void code_operand(const struct operand *o, int reg, bool hoist)
{
    if (o->tab < 0 && hoist) {
        init_consts[ninit++] = (struct hoisted){ reg, o->k };
    } else if (o->tab < 0) {
        emit_const(reg, &o->k);
    } else if (o->col == 0) {
        emit(OP_ROWID, o->tab, reg, 0);
    } else {
        emit(OP_COLUMN, o->tab, o->col, reg);
    }
}

static bool term_done[MAX_TERMS];
static int free_temp; /* a register that a comparison is done with, for the next one to reuse */

/* Code the terms that this level did not use to seek, and whose tables are all open by now. */
static void code_residual_terms(int level, unsigned open)
{
    for (int t = 0; t < q.nterm; t++) {
        const struct term *tm = &q.term[t];
        int lreg, rreg, a;
        if (term_done[t] || (term_mask(tm) & ~open) != 0) {
            continue;
        }
        term_done[t] = true;
        lreg = free_temp ? free_temp : ++nmem;
        free_temp = lreg;
        code_operand(&tm->l, lreg, false);
        rreg = ++nmem;
        code_operand(&tm->r, rreg, true);
        a = emit(skip_op[tm->op], rreg, -LABEL_CONT(level), lreg);
        snprintf(prog[a].p4, sizeof prog[a].p4, "BINARY-8");
        prog[a].p5 = 0x10 + (tables[q.from[tm->l.tab]].row[0].v[tm->l.col].text ? 0x42 : 0x44);
    }
}

/* The start of a level: the seek that finds the first row, and the check that ends the loop. */
static void code_level(int i)
{
    struct level *lv = &lev[i];
    const struct loop *lp = &loops[lv->loop];
    int neq = (lp->flags & W_EQ) ? 1 : 0;
    int brk = -LABEL_BRK(i);
    int base = nmem + 1, a;
    const struct cons *lo = NULL, *hi = NULL;
    int nstart = neq, nend = neq;
    for (int j = 0; j < lp->nl; j++) {
        term_done[cons[lp->lterm[j]].term] = true;
    }
    if (lp->idx < 0 && neq) {
        code_operand(cons_rhs(&cons[lp->lterm[0]]), base, false);
        nmem = base;
        emit(OP_SEEKROWID, lv->tabcur, brk, base);
        place(LABEL_CONT(i));
        return;
    }
    if (lp->idx < 0) {
        emit(OP_REWIND, lv->tabcur, brk, 0);
    } else {
        int nreg = neq + ((lp->flags & (W_BTM | W_TOP)) ? 1 : 0);
        int op;
        nmem += nreg;
        if (neq) {
            code_operand(cons_rhs(&cons[lp->lterm[0]]), base, false);
        }
        if (lp->flags & W_BTM) {
            lo = &cons[lp->lterm[0]];
            code_operand(cons_rhs(lo), base, false);
            nstart++;
        }
        if (lp->flags & W_TOP) {
            hi = &cons[lp->lterm[lp->nl - 1]];
        }
        if (nstart == 0) {
            op = OP_REWIND;
        } else if (!lo || cons_op(lo) == GE) {
            op = OP_SEEKGE;
        } else {
            op = OP_SEEKGT;
        }
        a = emit(op, lv->idxcur, brk, base);
        snprintf(prog[a].p4, sizeof prog[a].p4, "%d", nstart);
        if (hi) {
            code_operand(cons_rhs(hi), base, false);
            nend++;
        }
    }
    lv->top = nprog;
    if (nend > 0) {
        bool inclusive = !hi || cons_op(hi) == LE;
        a = emit(inclusive ? OP_IDXGT : OP_IDXGE, lv->idxcur, brk, base);
        snprintf(prog[a].p4, sizeof prog[a].p4, "%d", nend);
    }
    if (lp->idx >= 0 && !(lp->flags & W_IDX_ONLY)) {
        emit(OP_DEFERREDSEEK, lv->idxcur, 0, lv->tabcur);
    }
}

/* The end of a level: step to the next row, and the address where the loop is left. */
static void code_level_end(int i)
{
    const struct level *lv = &lev[i];
    const struct loop *lp = &loops[lv->loop];
    int a;
    if (!(lp->flags & W_IPK) || !(lp->flags & W_EQ)) {
        place(LABEL_CONT(i));
        a = emit(OP_NEXT, lp->idx >= 0 ? lv->idxcur : lv->tabcur, lv->top, 0);
        prog[a].p3 = (lp->flags & W_EQ) ? 1 : 0;
        prog[a].p5 = (lp->flags & W_CONSTRAINT) ? 0 : 1;
    }
    place(LABEL_BRK(i));
}

static void print_prog(void)
{
    for (int a = 0; a < nprog; a++) {
        const struct insn *x = &prog[a];
        fprintf(stderr, "  %-3d %-13s %-3d %-3d %-3d %-9s %d\n", a, op_name[x->op], x->p1,
                x->p2, x->p3, x->p4[0] ? x->p4 : "-", x->p5);
    }
}

static void stage_codegen(void)
{
    int ncur = q.nfrom, base, a;
    unsigned open = 0;
    emit(OP_INIT, 0, -LABEL_INIT, 0);
    for (int i = 0; i < q.nfrom; i++) {
        const struct loop *lp = &loops[lev[i].loop];
        lev[i].idxcur = lp->idx >= 0 ? ncur++ : -1;
        if (!(lp->flags & W_IDX_ONLY)) {
            a = emit(OP_OPENREAD, lev[i].tabcur, tables[q.from[lev[i].tabcur]].root, 0);
            snprintf(prog[a].p4, sizeof prog[a].p4, "%d", 32 - __builtin_clz(col_used[lev[i].tabcur]));
        }
        if (lp->idx >= 0) {
            a = emit(OP_OPENREAD, lev[i].idxcur, indexes[lp->idx].root, 0);
            snprintf(prog[a].p4, sizeof prog[a].p4, "k(2,,)");
            prog[a].p5 = (lp->flags & W_EQ) ? 2 : 0;
        }
    }
    for (int i = 0; i < q.nfrom; i++) {
        open |= 1u << loops[lev[i].loop].from;
        code_level(i);
        code_residual_terms(i, open);
    }
    base = nmem + 1;
    nmem += q.nsel;
    for (int i = 0; i < q.nsel; i++) {
        code_operand(&q.sel[i], base + i, false);
    }
    emit(OP_RESULTROW, base, q.nsel, 0);
    for (int i = q.nfrom - 1; i >= 0; i--) {
        code_level_end(i);
    }
    emit(OP_HALT, 0, 0, 0);
    place(LABEL_INIT);
    a = emit(OP_TRANSACTION, 0, 0, SCHEMA_COOKIE);
    snprintf(prog[a].p4, sizeof prog[a].p4, "0");
    prog[a].p5 = 1;
    for (int i = 0; i < ninit; i++) {
        emit_const(init_consts[i].reg, &init_consts[i].k);
    }
    emit(OP_GOTO, 0, 1, 0);

    fprintf(stderr, "== stage_codegen: nested loops, with every read on the table cursor ==\n");
    for (int a2 = 0; a2 < nprog; a2++) {
        if (prog[a2].p2 < 0) {
            prog[a2].p2 = label_addr[-prog[a2].p2];
        }
    }
    print_prog();

    /* An index loop reads the key column and the rowid from the index, which is cheaper than
     * seeking the table. A covering index loop does not open the table at all. */
    for (int i = 0; i < q.nfrom; i++) {
        const struct level *lv = &lev[i];
        if (lv->idxcur < 0) {
            continue;
        }
        for (int x = 0; x < nprog; x++) {
            struct insn *in = &prog[x];
            if (in->op == OP_COLUMN && in->p1 == lv->tabcur
                && in->p2 == indexes[loops[lv->loop].idx].col) {
                in->p1 = lv->idxcur;
                in->p2 = 0;
            } else if (in->op == OP_ROWID && in->p1 == lv->tabcur) {
                in->op = OP_IDXROWID;
                in->p1 = lv->idxcur;
            }
        }
    }
    fprintf(stderr, "after the index rewrite:\n");
    print_prog();
}

/* ---- the virtual machine ---- */

struct cursor {
    bool is_index;
    int id;       /* table or index */
    int pos;
    bool deferred;
    int target;   /* the cursor whose rowid a deferred seek is waiting for */
};

static struct cursor cur[MAX_CURS];
static struct value reg[MAX_REGS];

static long cursor_rowid(const struct cursor *c)
{
    return c->is_index ? indexes[c->id].e[c->pos].rowid : tables[c->id].row[c->pos].rowid;
}

static int cursor_count(const struct cursor *c)
{
    return c->is_index ? indexes[c->id].n : tables[c->id].nrow;
}

/* A deferred seek moves the table cursor only when a column of the table is really read. */
static void resolve_deferred(struct cursor *c)
{
    if (c->deferred) {
        long rowid = cursor_rowid(&cur[c->target]);
        int i = 0;
        while (tables[c->id].row[i].rowid != rowid) {
            i++;
        }
        c->pos = i;
        c->deferred = false;
    }
}

static const struct value *cursor_column(struct cursor *c, int col)
{
    if (c->is_index) {
        return &indexes[c->id].e[c->pos].key;
    }
    resolve_deferred(c);
    return &tables[c->id].row[c->pos].v[col];
}

/* Position an index cursor on the first entry at or after the key, or after it for a strict
 * seek. Returns false when there is none. */
static bool seek_index(struct cursor *c, const struct value *key, bool strict)
{
    const struct index *ix = &indexes[c->id];
    for (int i = 0; i < ix->n; i++) {
        int d = cmp_value(&ix->e[i].key, key);
        if (d > 0 || (d == 0 && !strict)) {
            c->pos = i;
            return true;
        }
    }
    return false;
}

static bool seek_rowid(struct cursor *c, long rowid)
{
    for (int i = 0; i < tables[c->id].nrow; i++) {
        if (tables[c->id].row[i].rowid == rowid) {
            c->pos = i;
            return true;
        }
    }
    return false;
}

static bool compare_jumps(int op, int d)
{
    switch (op) {
    case OP_NE:
        return d != 0;
    case OP_LE:
        return d <= 0;
    case OP_LT:
        return d < 0;
    case OP_GE:
        return d >= 0;
    default:
        return d > 0;
    }
}

static void stage_run(void)
{
    int pc = 0;
    fprintf(stderr, "== stage_run: instructions as they execute ==\n");
    for (;;) {
        const struct insn *x = &prog[pc];
        struct cursor *c = x->op >= OP_OPENREAD && x->op <= OP_ROWID ? &cur[x->p1] : NULL;
        int next = pc + 1;
        fprintf(stderr, "  %-3d %s\n", pc, op_name[x->op]);
        switch (x->op) {
        case OP_INIT:
        case OP_GOTO:
            next = x->p2;
            break;
        case OP_TRANSACTION:
            break;
        case OP_OPENREAD:
            *c = (struct cursor){ .is_index = x->p5 != 0 || x->p4[0] == 'k' };
            for (int i = 0; i < (c->is_index ? N_INDEXES : N_TABLES); i++) {
                if ((c->is_index ? indexes[i].root : tables[i].root) == x->p2) {
                    c->id = i;
                }
            }
            break;
        case OP_REWIND:
        case OP_NEXT:
            /* Rewind jumps when the table is empty, Next jumps while there are more rows. */
            c->pos = x->op == OP_REWIND ? 0 : c->pos + 1;
            if ((c->pos < cursor_count(c)) == (x->op == OP_NEXT)) {
                next = x->p2;
            }
            break;
        case OP_SEEKROWID:
            if (!seek_rowid(c, reg[x->p3].num)) {
                next = x->p2;
            }
            break;
        case OP_SEEKGE:
        case OP_SEEKGT:
            if (!seek_index(c, &reg[x->p3], x->op == OP_SEEKGT)) {
                next = x->p2;
            }
            break;
        case OP_IDXGE:
        case OP_IDXGT: {
            int d = cmp_value(&indexes[c->id].e[c->pos].key, &reg[x->p3]);
            if (d > 0 || (d == 0 && x->op == OP_IDXGE)) {
                next = x->p2;
            }
            break;
        }
        case OP_DEFERREDSEEK:
            cur[x->p3].deferred = true;
            cur[x->p3].target = x->p1;
            break;
        case OP_IDXROWID:
            reg[x->p2] = (struct value){ .num = cursor_rowid(c) };
            break;
        case OP_ROWID:
            resolve_deferred(c);
            reg[x->p2] = (struct value){ .num = cursor_rowid(c) };
            break;
        case OP_COLUMN:
            reg[x->p3] = *cursor_column(c, x->p2);
            break;
        case OP_INTEGER:
            reg[x->p2] = (struct value){ .num = x->p1 };
            break;
        case OP_STRING8:
            reg[x->p2] = (struct value){ .text = true, .str = x->p4 };
            break;
        case OP_RESULTROW:
            for (int i = 0; i < x->p2; i++) {
                if (i) {
                    putchar('|');
                }
                print_value(stdout, &reg[x->p1 + i]);
            }
            putchar('\n');
            break;
        case OP_HALT:
            return;
        default:
            if (compare_jumps(x->op, cmp_value(&reg[x->p3], &reg[x->p1]))) {
                next = x->p2;
            }
            break;
        }
        pc = next;
    }
}

/* ---- the first two stages ---- */

static void sort_entries(struct index *ix)
{
    for (int i = 1; i < ix->n; i++) {
        struct entry e = ix->e[i];
        int j = i;
        while (j > 0 && cmp_value(&ix->e[j - 1].key, &e.key) > 0) {
            ix->e[j] = ix->e[j - 1];
            j--;
        }
        ix->e[j] = e;
    }
}

/* SQLite guesses the width of a column from its type: text is 5, everything else is 1. */
static int col_width(int table, int col)
{
    return tables[table].row[0].v[col].text ? 5 : 1;
}

/* The catalog. The sizes are the defaults of SQLite for a database that was never ANALYZEd: a
 * million rows per table, and ten rows per distinct key. The widths are what is guessed from the
 * column types, and they make a covering index look cheaper than the table. */
static void stage_schema(void)
{
    fprintf(stderr, "== stage_schema: sizes in LogEst ==\n");
    for (int t = 0; t < N_TABLES; t++) {
        int sum = 0;
        for (int c = 0; c < tables[t].ncol; c++) {
            sum += col_width(t, c);
        }
        tables[t].nrowest = 200;
        tables[t].szrow = log_est((unsigned long long)sum * 4);
        fprintf(stderr, "  table %-12s root=%d rows=%d rowWidth=%d\n", tables[t].name,
                tables[t].root, tables[t].nrowest, tables[t].szrow);
    }
    for (int i = 0; i < N_INDEXES; i++) {
        struct index *ix = &indexes[i];
        const struct table *tb = &tables[ix->table];
        int width = col_width(ix->table, ix->col);
        ix->rowest[0] = tb->nrowest;
        ix->rowest[1] = 33;
        ix->szidx = log_est((unsigned long long)(width + 1) * 4);
        for (int r = 0; r < tb->nrow; r++) {
            ix->e[ix->n++] = (struct entry){ tb->row[r].v[ix->col], tb->row[r].rowid };
        }
        sort_entries(ix);
        fprintf(stderr, "  index %-12s root=%d rows=%d rowsPerKey=%d width=%d entries:", ix->name,
                ix->root, ix->rowest[0], ix->rowest[1], ix->szidx);
        for (int e = 0; e < ix->n; e++) {
            fprintf(stderr, " (");
            print_value(stderr, &ix->e[e].key);
            fprintf(stderr, ",%ld)", ix->e[e].rowid);
        }
        fprintf(stderr, "\n");
    }
}

/* The query as the planner sees it: each WHERE term is offered to the tables it mentions, and
 * the columns that the query reads decide whether an index can answer it alone. */
static void stage_query(void)
{
    fprintf(stderr, "== stage_query ==\n  %s\n", q.sql);
    for (int i = 0; i < q.nsel; i++) {
        use_column(&q.sel[i]);
    }
    for (int t = 0; t < q.nterm; t++) {
        const struct term *tm = &q.term[t];
        use_column(&tm->l);
        use_column(&tm->r);
        cons[ncons++] = (struct cons){ t, false };
        if (tm->r.tab >= 0) {
            cons[ncons++] = (struct cons){ t, true };
        }
        fprintf(stderr, "  term %d: ", t);
        print_operand(stderr, &tm->l);
        fprintf(stderr, " %s ", op_text[tm->op]);
        print_operand(stderr, &tm->r);
        fprintf(stderr, "\n");
    }
    for (int f = 0; f < q.nfrom; f++) {
        fprintf(stderr, "  %s reads columns mask %u\n", tname(f), col_used[f]);
    }
}

static const struct query *find_example(const char *name)
{
    for (size_t i = 0; i < sizeof examples / sizeof examples[0]; i++) {
        if (strcmp(name, examples[i].name) == 0) {
            return &examples[i].q;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const struct query *example;
    if (argc < 2 || (example = find_example(argv[1])) == NULL) {
        fprintf(stderr, "usage: %s <example> [code.bin]\n", argv[0]);
        return 2;
    }
    q = *example;
    stage_schema();
    stage_query();
    stage_loops();
    stage_solve();
    stage_codegen();
    stage_run();
    return 0;
}
