#!/usr/bin/env python3
"""Attribute recompiled functions to the source files they were compiled from.

The cartridge kept its asserts, and those asserts were compiled with `__FILE__`
intact, so the ROM contains the original source-tree paths. Each assert site
loads the address of one of those strings, which means every such site ties a
*function* to a *source file*.

That is worth a great deal here. The port carries roughly three thousand
functions named `func_800xxxxx`, and nothing else says which belong together.
This does not name any function, but it groups them, and a group with a name
like `Actions/Brain.c` tells you what its members are for.

The immediate use is narrowing a failure: when the game trips one of its own
assertions, the file the assert lives in is usually more informative than the
address.

Method:
  1. Find NUL-terminated source paths in the three loaded segments, and convert
     their ROM offsets to VRAM using the phase 01 segment map.
  2. Walk the disassembly tracking the current function, and resolve every
     address a `lui`/`addiu` or `lui`/`ori` pair materialises -- both splat's
     `%hi(D_x)/%lo(D_x)` form, where the symbol name carries the address, and
     raw immediates.
  3. Any function that materialises a source-path address is attributed to it.

Usage:  python tools/partition_by_file.py [--report docs/MODULE-MAP.md]
"""
import argparse
import collections
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

#            name    rom_start  rom_end    vram_start
SEGMENTS = [("boot", 0x001000, 0x01DCC0, 0x80000400),
            ("main", 0x01DCC0, 0x0C5BF0, 0x80025C50),
            ("aux",  0x0C5BF0, 0x0D0A20, 0x800F64A0)]

# A source path as a compiler would embed it: at least one path-ish character,
# ending in .c or .h. Deliberately strict -- loose patterns pull in fragments of
# unrelated data that merely end in ".c".
SOURCE_PATH = re.compile(rb"[A-Za-z0-9_][A-Za-z0-9_./\\-]{2,63}\.[ch]\x00")

INSN = re.compile(r"/\*\s+[0-9A-Fa-f]+\s+([0-9A-Fa-f]{8})\s+[0-9A-Fa-f]{8}\s+\*/\s+(.*)")
GLABEL = re.compile(r"^glabel (\S+)")

# Two ways an address reaches an instruction in splat's output, and both carry
# the WHOLE address rather than a half of it -- which makes this much easier
# than tracking lui/addiu register pairs:
#
#   symbolic:  addiu $a0, $a0, %lo(D_800C1234)   -- the name encodes the address
#   literal:   lui   $a0, (0x800C1234 >> 16)     -- the full constant is written
#                                                   out in both halves
SYM_REF = re.compile(r"%(?:hi|lo)\(([A-Za-z_][A-Za-z0-9_]*)\)")
ADDR_IN_NAME = re.compile(r"^[A-Za-z_]+_([0-9A-Fa-f]{8})$")
LITERAL_ADDR = re.compile(r"0x([0-9A-Fa-f]{6,8})")


def find_source_strings(rom: bytes) -> dict[int, str]:
    """VRAM address -> source path, for paths inside the loaded segments."""
    out: dict[int, str] = {}
    for _name, rom_start, rom_end, vram in SEGMENTS:
        chunk = rom[rom_start:rom_end]
        for m in SOURCE_PATH.finditer(chunk):
            text = m.group()[:-1].decode("ascii", "replace")
            # Reject obvious false positives: a real path has no spaces and is
            # not mostly punctuation.
            if " " in text or text.count(".") > 4:
                continue
            out[vram + m.start()] = text
    return out


def scan(asm_dir: pathlib.Path, strings: dict[int, str]):
    """function -> set of source files it references."""
    by_func: dict[str, set[str]] = collections.defaultdict(set)
    func_addr: dict[str, int] = {}

    for path in sorted(asm_dir.rglob("*.s")):
        cur = None
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            g = GLABEL.match(line)
            if g:
                cur = g.group(1)
                continue
            m = INSN.search(line)   # the line is indented; match() would anchor at column 0
            if not m or cur is None:
                continue
            vram, text = int(m.group(1), 16), m.group(2)
            func_addr.setdefault(cur, vram)

            for name in SYM_REF.findall(text):
                a = ADDR_IN_NAME.match(name)
                if a:
                    addr = int(a.group(1), 16)
                    if addr in strings:
                        by_func[cur].add(strings[addr])

            for lit in LITERAL_ADDR.findall(text):
                addr = int(lit, 16)
                if addr in strings:
                    by_func[cur].add(strings[addr])

    return by_func, func_addr


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=str(ROOT / "rom.z64"))
    ap.add_argument("--asm", default=str(ROOT / "asm"))
    ap.add_argument("--report", default=None)
    ap.add_argument("--func", default=None, help="report the file(s) for one function")
    args = ap.parse_args()

    rom = pathlib.Path(args.rom).read_bytes()
    asm_dir = pathlib.Path(args.asm)
    if not asm_dir.is_dir():
        raise SystemExit(f"{asm_dir} not found -- run scripts/split-rom.sh first")

    strings = find_source_strings(rom)
    by_func, func_addr = scan(asm_dir, strings)

    if args.func:
        files = by_func.get(args.func)
        print(f"{args.func}: {', '.join(sorted(files)) if files else '(no source file referenced)'}")
        return 0

    by_file: dict[str, list[str]] = collections.defaultdict(list)
    for fn, files in by_func.items():
        for f in files:
            by_file[f].append(fn)

    print(f"source paths embedded in the loaded segments : {len(strings)}")
    print(f"distinct source files referenced from code   : {len(by_file)}")
    print(f"functions attributed to at least one file    : {len(by_func)}")
    print()
    print(f"{'functions':>9}  source file")
    for f, fns in sorted(by_file.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        print(f"{len(fns):>9}  {f}")

    if args.report:
        out = pathlib.Path(args.report)
        out.parent.mkdir(parents=True, exist_ok=True)
        with out.open("w", newline="\n", encoding="utf-8") as fh:
            fh.write("# Module map\n\n")
            fh.write("Generated by `tools/partition_by_file.py` from the retained `__FILE__`\n"
                     "strings in the cartridge's asserts. Each entry lists the functions that\n"
                     "reference a given source path, which is to say the functions the compiler\n"
                     "built from that file and which contain at least one assert.\n\n"
                     "This names nothing. It groups, and a group with a name is worth far more\n"
                     "than three thousand `func_800xxxxx` in a flat list.\n\n")
            fh.write(f"- source paths embedded in the loaded segments: **{len(strings)}**\n")
            fh.write(f"- distinct source files referenced from code: **{len(by_file)}**\n")
            fh.write(f"- functions attributed: **{len(by_func)}**\n\n")
            for f, fns in sorted(by_file.items(), key=lambda kv: (-len(kv[1]), kv[0])):
                fh.write(f"## {f}  ({len(fns)} functions)\n\n")
                for fn in sorted(fns, key=lambda n: func_addr.get(n, 0)):
                    fh.write(f"- `{fn}`\n")
                fh.write("\n")
        print("wrote " + str(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
