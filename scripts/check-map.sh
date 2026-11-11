#!/usr/bin/env bash
# Check that design-space.svg labels every entry directory under distilled/ and original/.
# The label is the directory name. Usage: scripts/check-map.sh
set -euo pipefail
cd "$(dirname "$0")/.."

status=0
for dir in distilled/*/ original/*/; do
    name=$(basename "$dir")
    if ! grep -q ">$name</text>" design-space.svg; then
        echo "error: design-space.svg has no label for $dir" >&2
        status=1
    fi
done
((status == 0)) && echo "ok: every entry is on the map"
exit $status
