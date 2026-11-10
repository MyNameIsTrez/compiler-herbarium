#!/usr/bin/env bash
# Check that the "Lines of C" column of the README tables matches `cloc` for every entry's
# main.c, or rewrite the column with --update.
#
# Usage: scripts/check-cloc.sh [--update]
# Needs cloc. The number is cloc's count of code lines: no blank lines, no comments.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
update=0
[[ ${1:-} == "--update" ]] && update=1

status=0
tmp=$(mktemp)
while IFS= read -r line; do
    if [[ $line =~ ^\|\ (\[)?\`((distilled|original)/[a-z0-9-]+)\` ]]; then
        entry=${BASH_REMATCH[2]}
        if [[ ! -f $entry/main.c ]]; then
            echo "error: the README lists $entry, which has no main.c" >&2
            status=1
            printf '%s\n' "$line" >>"$tmp"
            continue
        fi
        want=$(cloc --csv --quiet "$entry/main.c" | tail -1 | cut -d, -f5)
        # The Lines of C cell is the one right after the entry cell.
        have=$(awk -F'|' '{gsub(/^ +| +$/, "", $3); print $3}' <<<"$line")
        if [[ $have != "$want" ]]; then
            if ((update)); then
                line=$(awk -F'|' -v OFS='|' -v w="$want" '{$3=" " w " "; print}' <<<"$line")
            else
                echo "error: $entry has $want lines of C, the README says $have" >&2
                status=1
            fi
        fi
    fi
    printf '%s\n' "$line" >>"$tmp"
done <README.md

if ((update)); then
    cat "$tmp" >README.md
    echo "updated README.md"
elif ((status == 0)); then
    echo "ok: the Lines of C column matches cloc"
fi
rm -f "$tmp"
exit $status
