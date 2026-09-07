#!/usr/bin/env bash
# Phase 02: translate the phase 01 ELF into C with N64Recomp.
#
# The `ignored` list -- data that splat placed in .text -- is derived from the
# ELF rather than maintained by hand, so it stays correct across re-splits. The
# effective config is generated into build/ and the hand-written config in
# recomp/ stays readable.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

ELF=elf/rayman2.us.elf
BASE=recomp/rayman2.us.toml
GEN=build/recomp/rayman2.us.toml

if [ ! -f "$ELF" ]; then
    echo "$ELF missing -- run scripts/build-elf.sh first" >&2
    exit 1
fi
if [ ! -x ./N64Recomp ]; then
    echo "./N64Recomp missing -- run scripts/build-recompiler.sh first" >&2
    exit 1
fi

echo "=== deriving the ignored list from the ELF ==="
python3 tools/gen_ignored_syms.py "$ELF"

echo "=== generating the effective config ==="
mkdir -p build/recomp RecompiledFuncs
# The generated config lives one directory deeper than the hand-written one, so
# its relative paths need another level of ..
awk -v list="recomp/ignored_syms.txt" '
    /^# @IGNORED@$/ {
        print "ignored = ["
        while ((getline line < list) > 0) print line
        print "]"
        next
    }
    { gsub(/"\.\.\//, "\"../../"); print }
' "$BASE" > "$GEN"
printf '  %s (%s lines, %s ignored symbols)\n' \
    "$GEN" "$(wc -l < "$GEN")" "$(wc -l < recomp/ignored_syms.txt)"

echo "=== recompiling ==="
rm -f RecompiledFuncs/*.c
./N64Recomp "$GEN"

echo "=== output ==="
echo "generated .c files : $(find RecompiledFuncs -name '*.c' | wc -l)"
echo "total size         : $(du -sh RecompiledFuncs | cut -f1)"
