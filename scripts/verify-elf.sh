#!/usr/bin/env bash
# Phase 01 gate: prove the assembled ELF's code is byte-identical to the ROM's.
#
# This is the check the whole phase exists for. An ELF that merely links proves
# nothing: a wrong function boundary or a mis-sized segment still assembles, and
# then corrupts state at run time. Byte-identity is the only evidence that the
# symbol table describes the cartridge rather than a plausible fiction.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

ELF=elf/rayman2.us.elf
ROM=rom.z64
OBJCOPY=mips-linux-gnu-objcopy

mkdir -p build/verify

#            section   ROM offset   size
SEGMENTS=(
    "boot     0x001000     0x01CCC0"
    "main     0x01DCC0     0x0A7F30"
    "aux      0x0C5BF0     0x00AE30"
)

fail=0
printf '%-6s  %-10s  %-9s  %s\n' SECTION "ROM OFF" SIZE RESULT
for entry in "${SEGMENTS[@]}"; do
    read -r name off size <<<"$entry"
    $OBJCOPY -O binary --only-section=".$name" "$ELF" "build/verify/$name.bin"
    dd if="$ROM" of="build/verify/$name.rom.bin" bs=1 \
       skip=$((off)) count=$((size)) status=none

    if cmp -s "build/verify/$name.bin" "build/verify/$name.rom.bin"; then
        printf '%-6s  %-10s  %-9s  IDENTICAL\n' "$name" "$off" "$size"
    else
        printf '%-6s  %-10s  %-9s  DIFFERS\n' "$name" "$off" "$size"
        cmp -l "build/verify/$name.bin" "build/verify/$name.rom.bin" | head -5 || true
        fail=1
    fi
done

echo
if [ "$fail" -eq 0 ]; then
    total=$(( 0x01CCC0 + 0x0A7F30 + 0x00AE30 ))
    echo "PHASE 01 GATE MET: all three code segments byte-identical ($total bytes)."
else
    echo "PHASE 01 GATE NOT MET." >&2
fi
exit "$fail"
