#!/usr/bin/env bash
# Build one entry, run it, check its output, and check (or regenerate) its committed dumps.
#
# Usage: scripts/check-entry.sh [--update] <entry-dir>
#
# Environment:
#   CC   C compiler, default gcc
#   OPT  optimization flag, default -O1
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
mkdir "$work/dumps"

"$cc" -std=c17 "$opt" -g -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    "$source_file" -o "$work/entry"

"$work/entry" "$work/dumps/code.bin" >"$work/stdout" 2>"$work/dumps/stages.txt"

# An entry that generates no machine code, such as an interpreter, leaves no code.bin.
if [[ ! -s $work/dumps/code.bin ]]; then
    rm -f "$work/dumps/code.bin"
fi

printf 'Hello, world!\n' >"$work/expected"
if ! diff -u "$work/expected" "$work/stdout"; then
    echo "FAIL $entry: stdout is not the expected output" >&2
    exit 1
fi

if [[ ! -s $work/dumps/stages.txt ]]; then
    echo "FAIL $entry: no stage dumps were written to stderr" >&2
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
