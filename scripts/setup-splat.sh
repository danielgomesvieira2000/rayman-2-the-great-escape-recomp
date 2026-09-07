#!/usr/bin/env bash
# Phase 01: prepare a Python environment for splat, in WSL or on Linux.
#
# Unlike the sibling ports there is no reference decomp to borrow a vendored
# splat from, so this installs a current splat64 release from PyPI and pins it,
# together with the disassembler stack it sits on. Pinning matters: splat's
# config schema moves between releases, and recomp/rayman2.us.yaml is written
# against the version below.
set -euo pipefail

VENV="${VENV:-$HOME/r2venv}"

if [ ! -d "$VENV" ]; then
    python3 -m venv "$VENV"
fi

"$VENV/bin/pip" install -q --upgrade pip
"$VENV/bin/pip" install -q \
    "splat64[mips]==0.32.2" \
    "PyYAML" \
    "pylibyaml" \
    "tqdm" \
    "intervaltree" \
    "colorama"

echo "--- installed ---"
"$VENV/bin/pip" list 2>/dev/null | grep -iE 'splat|spim|rabbit|yaml|interval' || true

echo "--- splat responds ---"
"$VENV/bin/python" -m splat --version 2>&1 | head -3
