#!/usr/bin/env bash
# Write kernels/<kernel>/expected.txt, or check that it is up to date with --check.
#
# Usage: scripts/gen-kernel.sh [--check] <kernel-dir>...
#
# A kernel is a function over one or two 32-bit words, in goal.txt:
#   line 1: "inputs N", the number of inputs (1 or 2), named a and b
#   line 2: one C expression over uint32_t a and b
# The script compiles that expression with gcc and evaluates it on the test vectors, so the
# expected output never rests on an entry. Never edit expected.txt by hand.
#
# The test vectors are every combination of the six edge values below (the first input varies
# fastest), then 16 pseudo-random vectors from a linear congruential generator seeded with 1.
# Each entry builds the same vectors by the same rule, and prints one line per vector.
set -euo pipefail

check=0
if [[ ${1:-} == "--check" ]]; then
    check=1
    shift
fi
if [[ $# -eq 0 ]]; then
    echo "usage: $0 [--check] <kernel-dir>..." >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
status=0

for dir in "$@"; do
    dir=${dir%/}
    arity=$(sed -n '1s/^inputs \([12]\)$/\1/p' "$dir/goal.txt")
    expr=$(sed -n '2p' "$dir/goal.txt")
    if [[ -z $arity || -z $expr ]]; then
        echo "error: $dir/goal.txt must be 'inputs 1' or 'inputs 2', then one C expression" >&2
        exit 2
    fi
    cat >"$work/gen.c" <<EOF
#include <stdint.h>
#include <stdio.h>
static uint32_t goal(uint32_t a, uint32_t b)
{
    (void)b;
    return (uint32_t)($expr);
}
static uint32_t state = 1;
static uint32_t lcg(void)
{
    state = state * 1664525u + 1013904223u;
    return state;
}
static void line(uint32_t a, uint32_t b)
{
#if $arity == 1
    printf("in0=%08x -> %08x\n", a, goal(a, b));
#else
    printf("in0=%08x in1=%08x -> %08x\n", a, b, goal(a, b));
#endif
}
int main(void)
{
    const uint32_t edge[6] = {0, 1, 2, 0x7fffffffu, 0x80000000u, 0xffffffffu};
#if $arity == 1
    for (int i = 0; i < 6; i++) {
        line(edge[i], 0);
    }
#else
    for (int j = 0; j < 6; j++) {
        for (int i = 0; i < 6; i++) {
            line(edge[i], edge[j]);
        }
    }
#endif
    for (int k = 0; k < 16; k++) {
        uint32_t a = lcg();
        uint32_t b = $arity == 2 ? lcg() : 0;
        line(a, b);
    }
    return 0;
}
EOF
    gcc -std=c17 -Wall -Wextra -Werror "$work/gen.c" -o "$work/gen"
    "$work/gen" >"$work/expected.txt"
    if ((check)); then
        if ! diff -u "$dir/expected.txt" "$work/expected.txt"; then
            echo "FAIL $dir: expected.txt is out of date, run: $0 $dir" >&2
            status=1
        fi
    else
        cp "$work/expected.txt" "$dir/expected.txt"
        echo "wrote $dir/expected.txt"
    fi
done
exit $status
