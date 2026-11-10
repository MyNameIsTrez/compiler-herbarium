#!/usr/bin/env bash
# Build one entry, run it on every example, check its output, and check (or regenerate) its
# committed dumps.
#
# Usage: scripts/check-entry.sh [--update] <entry-dir>
#
# Environment:
#   CC   C compiler, default gcc
#   OPT  optimization flag, default -O1
#
# An entry is run as: ./entry <example> [code.bin]
#   stdout          must equal examples/<example>/expected.txt
#   stderr          the intermediate form after each stage, committed as dumps/<example>/stages.txt
#   code.bin        the final machine code, committed as dumps/<example>/code.bin, if the entry
#                   generates machine code
#   no example, or an unknown one: must exit with status 2
set -euo pipefail

update=0
if [[ ${1:-} == "--update" ]]; then
    update=1
    shift
fi

if [[ $# -ne 1 ]]; then
    echo "usage: $0 [--update] <entry-dir>" >&2
    exit 2
fi

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
entry=${1%/}
source_file="$entry/main.c"
if [[ ! -f $source_file ]]; then
    echo "error: $source_file does not exist" >&2
    exit 2
fi

cc=${CC:-gcc}
opt=${OPT:--O1}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# clang's -fsanitize=function reads a type tag in the 8 bytes before the target of every call
# through a function pointer. Entries that run generated machine code call into an mmap'd page,
# where those bytes are not mapped, so the check would crash on correct code. gcc has no such
# check, and does not know the flag.
extra=()
if "$cc" --version | grep -qi clang; then
    extra+=(-fno-sanitize=function)
fi

"$cc" -std=c17 "$opt" -g -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined "${extra[@]}" \
    "$source_file" -o "$work/entry"

status=0
shopt -s nullglob
examples=("$root"/examples/*/)
if [[ ${#examples[@]} -eq 0 ]]; then
    echo "error: no examples found in $root/examples" >&2
    exit 2
fi

for example_dir in "${examples[@]}"; do
    example=$(basename "$example_dir")
    out="$work/dumps/$example"
    mkdir -p "$out"

    if ! "$work/entry" "$example" "$out/code.bin" >"$out/stdout" 2>"$out/stages.txt"; then
        echo "FAIL $entry $example: the entry exited with an error" >&2
        status=1
        continue
    fi

    # An entry that generates no machine code, such as an interpreter, leaves no code.bin.
    if [[ ! -s $out/code.bin ]]; then
        rm -f "$out/code.bin"
    fi

    if ! diff -u "$example_dir/expected.txt" "$out/stdout"; then
        echo "FAIL $entry $example: stdout is not the expected output" >&2
        status=1
    fi
    rm -f "$out/stdout"

    if [[ ! -s $out/stages.txt ]]; then
        echo "FAIL $entry $example: no stage dumps were written to stderr" >&2
        status=1
    fi
done

# A missing or unknown example must be rejected with status 2.
for args in "" "no-such-example"; do
    set +e
    # shellcheck disable=SC2086
    "$work/entry" $args >/dev/null 2>&1
    code=$?
    set -e
    if [[ $code -ne 2 ]]; then
        echo "FAIL $entry: running with '${args:-no arguments}' exited with $code, expected 2" >&2
        status=1
    fi
done

if [[ $status -ne 0 ]]; then
    exit 1
fi

if [[ $update -eq 1 ]]; then
    rm -rf "$entry/dumps"
    cp -r "$work/dumps" "$entry/dumps"
    echo "updated $entry/dumps"
    exit 0
fi

if [[ ! -d $entry/dumps ]]; then
    echo "FAIL $entry: no committed dumps, run: $0 --update $entry" >&2
    exit 1
fi

if ! diff -r "$work/dumps" "$entry/dumps"; then
    echo "FAIL $entry: committed dumps are out of date, run: $0 --update $entry" >&2
    exit 1
fi

echo "ok $entry"
