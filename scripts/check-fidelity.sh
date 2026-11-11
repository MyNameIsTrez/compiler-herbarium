#!/usr/bin/env bash
# Check that the stages in an entry's main.c and in its FIDELITY.md agree, and that FIDELITY.md
# says how the entry's expected.txt files are regenerated.
#
# Usage: scripts/check-fidelity.sh <entry-dir>
#
# Every function named stage_* that main.c defines or declares at the start of a line must
# appear in FIDELITY.md as `stage_name` in backticks, and every such name in FIDELITY.md must
# exist in main.c. This only catches a stage that was added, removed or renamed in one place
# but not the other. Whether a deviation entry is still accurate is a human judgment.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 <entry-dir>" >&2
    exit 2
fi

entry=${1%/}
for file in "$entry/main.c" "$entry/FIDELITY.md"; do
    if [[ ! -f $file ]]; then
        echo "error: $file does not exist" >&2
        exit 2
    fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# Function definitions and prototypes start at the beginning of a line, calls are indented.
{ grep -oE '^[A-Za-z_][A-Za-z0-9_ *]*[ *]stage_[a-z0-9_]+\(' "$entry/main.c" |
    grep -oE 'stage_[a-z0-9_]+' | sort -u; } >"$work/in_c" || true
{ grep -oE '`stage_[a-z0-9_]+`' "$entry/FIDELITY.md" |
    tr -d '`' | sort -u; } >"$work/in_doc" || true

if [[ ! -s $work/in_c ]]; then
    echo "FAIL $entry: main.c has no stage_* functions" >&2
    exit 1
fi

status=0
while read -r name; do
    echo "FAIL $entry: $name is in main.c but not in FIDELITY.md" >&2
    status=1
done < <(comm -23 "$work/in_c" "$work/in_doc")
while read -r name; do
    echo "FAIL $entry: $name is in FIDELITY.md but not in main.c" >&2
    status=1
done < <(comm -13 "$work/in_c" "$work/in_doc")

if ! grep -q '^## Regenerating expected.txt$' "$entry/FIDELITY.md"; then
    echo "FAIL $entry: FIDELITY.md has no '## Regenerating expected.txt' section" >&2
    status=1
fi

if [[ $status -eq 0 ]]; then
    echo "ok $entry: stages in main.c and FIDELITY.md agree"
fi
exit "$status"
