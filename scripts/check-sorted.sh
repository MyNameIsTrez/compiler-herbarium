#!/usr/bin/env bash
# Check that the rows of every "Entry" and "Lead" table in README.md and RESEARCH.md are sorted
# alphabetically by their first column (case-insensitive, link and code markup ignored).
# Usage: scripts/check-sorted.sh [--print]   (--print shows each table's keys in sorted order)
set -euo pipefail
cd "$(dirname "$0")/.."

key() {
    local cell=${1#|}
    cell=${cell%%|*}
    cell=$(sed -E 's/\[([^]]*)\]\([^)]*\)/\1/g; s/`//g; s/^ +//; s/ +$//' <<<"$cell")
    tr '[:upper:]' '[:lower:]' <<<"$cell"
}

status=0
check_table() {
    local file=$1 start=$2; shift 2
    local keys=("$@")
    local sorted
    sorted=$(printf '%s\n' "${keys[@]}" | LC_ALL=C sort)
    if [[ "$(printf '%s\n' "${keys[@]}")" != "$sorted" ]]; then
        echo "error: $file: the table at line $start is not sorted alphabetically by its first column" >&2
        status=1
    fi
}

for file in README.md RESEARCH.md; do
    in_table=0 keys=() start=0 lineno=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        lineno=$((lineno + 1))
        if [[ $line == '|'* ]]; then
            if ((in_table == 0)); then
                in_table=1 keys=() start=$lineno
                first=$(key "$line")
                [[ $first == entry || $first == lead ]] && in_table=2
            elif ((in_table == 2)) && [[ $line != '|--'* ]]; then
                keys+=("$(key "$line")")
            fi
        else
            ((in_table == 2)) && check_table "$file" "$start" "${keys[@]}"
            in_table=0
        fi
    done <"$file"
    ((in_table == 2)) && check_table "$file" "$start" "${keys[@]}"
done
[[ $status -eq 0 ]] && echo "ok: tables are sorted"
exit $status
