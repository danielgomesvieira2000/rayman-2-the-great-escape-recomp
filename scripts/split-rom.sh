#!/usr/bin/env bash
# Phase 01: split the ROM into assembly with splat.
#
# Produces asm/ (per-function .s), recomp/rayman2.us.ld (the linker script) and
# the auto-symbol lists. All of it is derived from the builder's own dump and is
# git-ignored; nothing here is committed.
set -euo pipefail

VENV="${VENV:-$HOME/r2venv}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

if [ ! -f rom.z64 ]; then
    echo "rom.z64 missing from $REPO -- supply your own dump" >&2
    exit 1
fi

echo "=== splat ==="
"$VENV/bin/python" -m splat split recomp/rayman2.us.yaml "$@" 2>&1 \
    | grep -vE 'it/s\]|s/it\]' || true

echo
echo "=== output ==="
echo "asm .s files    : $(find asm -name '*.s' 2>/dev/null | wc -l)"
echo "function labels : $(grep -rho '^glabel [A-Za-z_][A-Za-z0-9_]*' asm 2>/dev/null | wc -l)"
echo "linker script   : $(wc -l < recomp/rayman2.us.ld 2>/dev/null || echo missing) lines"
