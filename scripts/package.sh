#!/usr/bin/env bash
# Assemble a distributable build, named for the version it was built from.
#
# The first two releases were put together by hand, and by the second one the
# question "what actually has to be in the folder" had already been answered
# twice from memory. This is that answer written down.
#
# What goes in, and what deliberately does not:
#
#   rayman2-recomp.exe          the port
#   SDL2.dll                    window, input and audio
#   dxcompiler.dll, dxil.dll    RT64 compiles shaders at runtime through these
#   assets/                     fonts, menu icons and the frontend stylesheet,
#                               all loaded from disk by name -- a missing file
#                               here is a missing button rather than an error
#   README, LICENSE, notices    what it is, and what it is built on
#
#   shaders/                    NOT included. That directory is a build
#                               artefact: the .h and .c beside each .dxil are
#                               compiled into the executable, and nothing reads
#                               the directory at run time.
#   mods/, mod_config/, saves/  NOT included. Created on demand, and shipping
#                               an empty one says they are meant to be
#                               populated by hand.
#
# The version comes from src/main.cpp, which is the only place it is written
# down -- so a release cannot be named one thing and report itself as another.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

BUILD="build-cmake"
while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD="$2"; shift 2 ;;
        -h|--help)
            echo "usage: scripts/package.sh [--build DIR]   (default $BUILD)" >&2
            exit 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Read the version out of the source rather than taking it on the command line.
# A packaged build whose folder name disagrees with its own BUILD block is worse
# than no name at all, and that cannot happen if nobody gets to type it.
read -r MAJOR MINOR PATCH SUFFIX <<EOF
$(sed -n 's/^const recomp::Version kProjectVersion{ *\([0-9]*\), *\([0-9]*\), *\([0-9]*\), *"\([^"]*\)".*/\1 \2 \3 \4/p' src/main.cpp)
EOF
if [ -z "${MAJOR:-}" ]; then
    echo "could not read kProjectVersion out of src/main.cpp" >&2
    exit 1
fi
VERSION="${MAJOR}.${MINOR}.${PATCH}${SUFFIX}"

# The build must be an optimised one with the frontend on. Release and
# RelWithDebInfo both qualify, and both report themselves as "release" in a
# session report, because that line is decided by NDEBUG and NDEBUG is defined
# in both. RelWithDebInfo is preferred: it is the same optimisation with a PDB
# beside it, and a PDB is what turns the addresses in a player's crash block
# into function names.
CONFIG="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
FRONTEND="$(sed -n 's/^RAYMAN2_ENABLE_FRONTEND:BOOL=//p' "$BUILD/CMakeCache.txt" 2>/dev/null || true)"
if { [ "$CONFIG" != "Release" ] && [ "$CONFIG" != "RelWithDebInfo" ]; } || [ "$FRONTEND" != "ON" ]; then
    echo "$BUILD is ${CONFIG:-?} with frontend ${FRONTEND:-?};" >&2
    echo "a release needs Release or RelWithDebInfo, with the frontend ON" >&2
    exit 1
fi

NAME="rayman-2-the-great-escape-recomp-v${VERSION}-windows-x64"
OUT="$ROOT/dist/$NAME"
rm -rf "$OUT"
mkdir -p "$OUT"

for f in rayman2-recomp.exe SDL2.dll dxcompiler.dll dxil.dll; do
    if [ ! -f "$BUILD/$f" ]; then
        echo "missing $BUILD/$f -- build first" >&2
        exit 1
    fi
    cp "$BUILD/$f" "$OUT/"
done
cp -r "$BUILD/assets" "$OUT/assets"
cp README.md LICENSE THIRD_PARTY_NOTICES.md "$OUT/"

# Whatever this machine has. Git Bash on Windows ships neither zip nor 7z, but
# it always has PowerShell, and Windows is the platform being packaged for.
( cd "$ROOT/dist" && rm -f "$NAME.zip"
  if command -v zip >/dev/null 2>&1; then
      zip -qr "$NAME.zip" "$NAME"
  elif command -v powershell.exe >/dev/null 2>&1; then
      powershell.exe -NoProfile -NonInteractive -Command           "Compress-Archive -Path '$NAME' -DestinationPath '$NAME.zip' -Force" >/dev/null
  else
      echo "no zip and no powershell -- the folder is built, archive it by hand" >&2
  fi )

# Symbols, as a separate download.
#
# They must come from the SAME link as the executable being shipped. A PDB from
# a different build of the same source resolves addresses to confident, wrong
# answers -- which is the failure 0.2.0-alpha's notes describe having already
# been caught once, in the build stamp.
if [ -f "$BUILD/rayman2-recomp.pdb" ]; then
    SYMDIR="$ROOT/dist/${NAME%-windows-x64}-symbols"
    rm -rf "$SYMDIR"
    mkdir -p "$SYMDIR"
    cp "$BUILD/rayman2-recomp.pdb" "$SYMDIR/"
    ( cd "$ROOT/dist" && rm -f "$(basename "$SYMDIR").zip"
      if command -v zip >/dev/null 2>&1; then
          zip -qr "$(basename "$SYMDIR").zip" "$(basename "$SYMDIR")"
      elif command -v powershell.exe >/dev/null 2>&1; then
          powershell.exe -NoProfile -NonInteractive -Command               "Compress-Archive -Path '$(basename "$SYMDIR")' -DestinationPath '$(basename "$SYMDIR").zip' -Force" >/dev/null
      fi )
    rm -rf "$SYMDIR"
    echo "  symbols: $SYMDIR.zip"
else
    echo "  NO SYMBOLS: $BUILD has no .pdb, so a crash address in a player's" >&2
    echo "  report cannot be resolved to a function. Build RelWithDebInfo." >&2
fi

echo "packaged $VERSION from $BUILD ($CONFIG, frontend $FRONTEND)"
echo "  $OUT"
echo "  $OUT.zip"
du -sh "$OUT" "$OUT.zip" | sed 's/^/  /'
