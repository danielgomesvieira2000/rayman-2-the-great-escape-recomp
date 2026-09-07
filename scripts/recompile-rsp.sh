#!/usr/bin/env bash
# Phase 05: recompile the audio microcode with RSPRecomp.
#
# Separate from scripts/recompile.sh because it is a different tool with a
# different input: N64Recomp translates the CPU code out of the ELF, RSPRecomp
# translates a block of RSP instructions straight out of the ROM. Nothing here
# depends on the split or on the ELF.
#
# The generated file is C++ -- it includes librecomp's rsp headers and uses
# attributes and value initialisation -- so it is named .cpp and compiled as
# such. Naming it .c and letting CMake infer the language does not work.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

if [ ! -f rom.z64 ]; then
    echo "rom.z64 not found -- this phase needs the cartridge dump" >&2
    exit 1
fi
if [ ! -x build/recompiler/RSPRecomp ]; then
    echo "RSPRecomp not built -- run: bash scripts/build-recompiler.sh" >&2
    exit 1
fi

# RSPRecomp resolves paths against its own working directory rather than the
# config, so the config carries absolute ones and they are rewritten here to
# match wherever the tree actually sits.
CFG="$(mktemp)"
trap 'rm -f "$CFG"' EXIT
sed -e "s|^rom_file_path .*|rom_file_path        = \"$ROOT/rom.z64\"|" \
    -e "s|^output_file_path .*|output_file_path     = \"$ROOT/RecompiledRsp/rsp_audio.cpp\"|" \
    recomp/rsp_audio.us.toml > "$CFG"

mkdir -p RecompiledRsp
./build/recompiler/RSPRecomp "$CFG"

lines=$(wc -l < RecompiledRsp/rsp_audio.cpp)
echo "generated RecompiledRsp/rsp_audio.cpp : $lines lines"
if [ "$lines" -lt 100 ]; then
    echo "suspiciously short -- check text_offset and text_size" >&2
    exit 1
fi
