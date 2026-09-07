#!/usr/bin/env bash
# Phase 02 gate: compile the recompiled C and archive it into a static library.
#
# This proves the recompiler's output is well-formed C that a host compiler
# accepts -- the entry condition for phase 03, where it gets linked against the
# real runtime. It does NOT link against librecomp yet, so it proves nothing
# about behaviour; that is phase 04's job.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

CC="${CC:-cc}"
JOBS="${JOBS:-$(nproc)}"
OBJDIR=build/recompiled
LIB=build/librayman2_recompiled.a

# -fno-strict-aliasing: the generated code reads and writes RDRAM through
#   differently-typed lvalues by design.
# -Wno-* : the output is machine-generated; these fire in bulk and mean nothing.
# -include port_runtime.h: declares the handful of functions N64Recomp drops
#   and leaves to the port. The generated sources include only recomp.h and
#   funcs.h, and must never be hand-edited, so the declaration is forced in.
CFLAGS="-c -O1 -std=gnu17 -fno-strict-aliasing -I lib/N64Recomp/include -I RecompiledFuncs"
CFLAGS="$CFLAGS -I include -include include/port_runtime.h"
CFLAGS="$CFLAGS -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses"

if ! ls RecompiledFuncs/*.c >/dev/null 2>&1; then
    echo "RecompiledFuncs/ is empty -- run scripts/recompile.sh first" >&2
    exit 1
fi

rm -rf "$OBJDIR"; mkdir -p "$OBJDIR"

echo "=== compiling $(ls RecompiledFuncs/*.c | wc -l) files with $CC (-j$JOBS) ==="
fail=0
printf '%s\n' RecompiledFuncs/*.c \
  | xargs -P "$JOBS" -I{} sh -c \
      "$CC $CFLAGS \"\$1\" -o $OBJDIR/\$(basename \"\$1\" .c).o" _ {} || fail=1

if [ "$fail" -ne 0 ]; then
    echo "PHASE 02 GATE NOT MET: compilation failed." >&2
    exit 1
fi

echo "=== archiving ==="
ar rcs "$LIB" "$OBJDIR"/*.o

n_obj=$(ls "$OBJDIR"/*.o | wc -l)
n_sym=$(nm --defined-only "$LIB" 2>/dev/null | grep -c ' T ' || true)
echo
echo "objects        : $n_obj"
echo "library        : $LIB ($(du -h "$LIB" | cut -f1))"
echo "exported funcs : $n_sym"
echo
echo "PHASE 02 GATE MET: the recompiled C compiles and archives cleanly."
