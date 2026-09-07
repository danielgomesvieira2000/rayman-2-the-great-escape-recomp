#!/usr/bin/env bash
# Phase 02: build the N64Recomp static recompiler (and RSPRecomp) from lib/.
#
# This builds the TOOL, not the port. It runs wherever a C++20 compiler exists;
# the resulting binaries are used to translate the phase 01 ELF into C. They are
# build artifacts and are git-ignored.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

BUILD_DIR="${BUILD_DIR:-build/recompiler}"

if [ ! -f lib/N64Recomp/CMakeLists.txt ]; then
    echo "lib/N64Recomp is empty -- run: git submodule update --init --recursive" >&2
    exit 1
fi

echo "=== configuring ==="
cmake -S lib/N64Recomp -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "=== building ==="
cmake --build "$BUILD_DIR" -j --target N64Recomp RSPRecomp

echo "=== installing to repo root ==="
for t in N64Recomp RSPRecomp; do
    src="$(find "$BUILD_DIR" -maxdepth 2 -type f -name "$t" -perm -u+x | head -1)"
    if [ -n "$src" ]; then
        cp "$src" "./$t"
        printf '  %s -> ./%s\n' "$src" "$t"
    else
        echo "  WARNING: $t not found in $BUILD_DIR" >&2
    fi
done

./N64Recomp --help 2>&1 | head -5 || true
