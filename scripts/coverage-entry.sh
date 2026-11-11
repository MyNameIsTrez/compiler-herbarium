#!/usr/bin/env bash
# Build one entry with coverage instrumentation, run it, and require 100% line and branch coverage.
#
# Usage: scripts/coverage-entry.sh <entry-dir>
#
# Needs gcc and gcovr (pip install gcovr==8.8).
#
# The coverage build is separate from scripts/check-entry.sh on purpose. It uses -O0 and no
# sanitizers, because the optimizer and the sanitizers both add compiler-generated branches
# that no test can take, which would make 100% branch coverage unreachable.
#
# A line or branch that can never run, such as the failure path of a syscall, may be excluded
# with one of these comments, and each use should say why:
#   // GCOVR_EXCL_LINE
#   // GCOVR_EXCL_BR_LINE
#   // GCOVR_EXCL_START ... // GCOVR_EXCL_STOP
#   // GCOVR_EXCL_BR_START ... // GCOVR_EXCL_BR_STOP
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <entry-dir>" >&2
    exit 2
fi

entry=${1%/}
if [[ ! -f $entry/main.c ]]; then
    echo "error: $entry/main.c does not exist" >&2
    exit 2
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$entry/main.c" "$work/main.c"

shopt -s nullglob
examples=("$entry/examples"/*/)
if [[ ${#examples[@]} -eq 0 ]]; then
    echo "error: no examples found in $entry/examples" >&2
    exit 2
fi

(
    cd "$work"
    gcc -std=c17 -O0 -g -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes \
        --coverage main.c -o entry

    # Coverage accumulates over runs. Every example is run once with a code.bin path, as
    # scripts/check-entry.sh does, and the first one once more without it.
    for example_dir in "${examples[@]}"; do
        ./entry "$(basename "$example_dir")" code.bin >/dev/null 2>&1
    done
    ./entry "$(basename "${examples[0]}")" >/dev/null 2>&1

    # A missing or unknown example is rejected with status 2.
    for args in "" "no-such-example"; do
        set +e
        # shellcheck disable=SC2086
        ./entry $args >/dev/null 2>&1
        set -e
    done
)

gcovr --root "$work" --object-directory "$work" --filter "$work/main.c" \
    --txt-metric branch --print-summary --fail-under-line 100 --fail-under-branch 100
echo "ok $entry: 100% line and branch coverage"
