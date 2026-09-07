#!/usr/bin/env bash
# Phase 02: recover call targets splat did not classify as functions, and
# iterate until the symbol set stops growing.
#
# Each round can expose code that was previously sitting undecoded inside a
# region splat treated as data, which in turn exposes more call targets, so a
# single pass is not enough. This converges in a handful of rounds.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

MAX_ROUNDS="${MAX_ROUNDS:-8}"

for round in $(seq 1 "$MAX_ROUNDS"); do
    echo "===== round $round ====="
    out="$(python3 tools/gen_missing_funcs.py rom.z64 elf/rayman2.us.elf)"
    echo "$out"
    n="$(echo "$out" | sed -n 's/^newly declared *: *\([0-9]*\).*/\1/p')"
    if [ "$n" = "0" ]; then
        echo "converged after $round round(s): no unclassified call targets left."
        exit 0
    fi
    rm -rf asm
    bash scripts/split-rom.sh  > /dev/null 2>&1
    bash scripts/build-elf.sh  > /dev/null 2>&1
done

echo "still finding new functions after $MAX_ROUNDS rounds -- investigate." >&2
exit 1
