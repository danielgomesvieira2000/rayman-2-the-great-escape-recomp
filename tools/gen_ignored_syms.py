#!/usr/bin/env python3
"""List data symbols that sit in executable sections, for N64Recomp to ignore.

N64Recomp treats every function symbol in an executable section as something to
translate. splat, run with `disassemble_all`, puts a segment's data blobs --
jump tables, float pools, pointer tables -- inside the same `.text` as the code,
and labels them with `glabel` exactly as it labels functions. Our `macro.inc`
types every `glabel` as `@function`, so the ELF cannot distinguish them either,
and the recompiler tries to decode data as instructions. It fails loudly when
the bytes are an unimplementable encoding, and -- far worse -- silently produces
wrong code when they happen to decode cleanly.

The discriminator is splat's naming convention: `D_<addr>` (and the jump-table
prefixes) for data, everything else for code.

Select on the DATA names, never on "not func_". Hand-verified function names
from recomp/symbol_addrs.txt -- `osGetCount` and friends -- are code, and
sweeping them in here makes N64Recomp reject the config outright, because a
libultra function it recognises is reimplemented by the runtime and so is not
in its function list to be ignored.

Usage:  python tools/gen_ignored_syms.py elf/rayman2.us.elf
        (writes recomp/ignored_syms.txt, spliced into the config by
         scripts/recompile.sh)
"""
import pathlib
import re
import subprocess
import sys

READELF = "mips-linux-gnu-readelf"
CODE_SECTIONS = {".boot", ".main", ".aux"}
OUT = pathlib.Path("recomp/ignored_syms.txt")

# splat's generated names for data it could not attribute to a function.
DATA_NAME = re.compile(r"^(?:D|jtbl|jpt)_[0-9A-Fa-f]{8}$")


def section_indices(elf: str) -> dict[str, str]:
    out = subprocess.run([READELF, "-SW", elf], capture_output=True, text=True, check=True).stdout
    idx = {}
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)", line)
        if m and m.group(2) in CODE_SECTIONS:
            idx[m.group(2)] = m.group(1)
    return idx


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <elf>")
    elf = sys.argv[1]

    secs = section_indices(elf)
    if not secs:
        raise SystemExit(f"error: none of {sorted(CODE_SECTIONS)} found in {elf}")
    want = set(secs.values())

    out = subprocess.run([READELF, "-sW", elf], capture_output=True, text=True, check=True).stdout

    code, ignored = 0, {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 8 or not parts[0].endswith(":"):
            continue
        _, value, _size, typ, _bind, _vis, ndx, name = parts[:8]
        if ndx not in want or typ == "SECTION":
            continue
        if DATA_NAME.match(name):
            ignored.setdefault(name, int(value, 16))
        elif typ == "FUNC":
            code += 1

    order = sorted(ignored.items(), key=lambda kv: kv[1])

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", newline="\n") as f:
        for name, _addr in order:
            f.write(f'    "{name}",\n')

    print(f"code sections   : {', '.join(f'{k}={v}' for k, v in sorted(secs.items()))}")
    print(f"function symbols: {code}")
    print(f"ignored (data)  : {len(order)}  -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
