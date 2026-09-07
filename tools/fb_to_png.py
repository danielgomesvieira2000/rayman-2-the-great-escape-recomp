#!/usr/bin/env python3
"""Turn a raw N64 framebuffer dump into a PNG.

The port can say how many display lists it submitted, but not what they drew,
and a black window is equally consistent with the game drawing nothing and with
the port failing to present what it drew. This closes that gap: src/rt64_context
writes the bytes the VI is pointed at, and this makes them viewable.

Two details matter.

RDRAM is stored word-swapped -- each 32-bit word is held in host order, so byte
i of the big-endian word lives at index i^3. Reading the dump linearly gives
scrambled pixels; the unswap below is not cosmetic.

The framebuffer is RGBA5551: five bits each of red, green and blue and one of
coverage, big-endian within the halfword. The five-bit channels are scaled to
eight by replicating the high bits, which is what the hardware's DAC does and
what makes greys come out grey rather than dark.

Usage:  python tools/fb_to_png.py fb_dump.bin fb.png [width] [height]
"""
import struct
import sys
import zlib


def main(argv):
    src = argv[1] if len(argv) > 1 else "fb_dump.bin"
    dst = argv[2] if len(argv) > 2 else "fb.png"
    w = int(argv[3]) if len(argv) > 3 else 320
    h = int(argv[4]) if len(argv) > 4 else 240

    raw = open(src, "rb").read()
    need = w * h * 2
    if len(raw) < need:
        raise SystemExit(f"{src}: {len(raw)} bytes, need {need}")

    # Undo the word swap, then decode RGBA5551.
    un = bytearray(need)
    for i in range(need):
        un[i] = raw[i ^ 3]

    rows = bytearray()
    for y in range(h):
        rows.append(0)                      # PNG filter: none
        base = y * w * 2
        for x in range(w):
            px = (un[base + x * 2] << 8) | un[base + x * 2 + 1]
            r5, g5, b5 = (px >> 11) & 31, (px >> 6) & 31, (px >> 1) & 31
            rows += bytes(((r5 << 3) | (r5 >> 2),
                           (g5 << 3) | (g5 >> 2),
                           (b5 << 3) | (b5 >> 2)))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
           + chunk(b"IEND", b""))
    open(dst, "wb").write(png)

    nonblack = sum(1 for i in range(0, need, 2)
                   if ((un[i] << 8) | un[i + 1]) & 0xFFC0)
    print(f"wrote {dst}  {w}x{h}  non-black pixels: {nonblack} of {w*h}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
