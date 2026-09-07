# Phase 01 findings — splitting the ROM

**Gate met.** An assembly-only ELF assembles and links, and all three of its
code sections are byte-identical to the cartridge's code: **850,464 bytes**
across `.boot`, `.main` and `.aux`. Reproduce with:

```bash
scripts/setup-splat.sh      # once, in WSL or on Linux
scripts/split-rom.sh        # splat -> asm/, recomp/rayman2.us.ld
scripts/build-elf.sh        # assemble + link -> elf/rayman2.us.elf
scripts/verify-elf.sh       # the gate
```

## The headline: phase 00 was wrong about the code layout

Phase 00 concluded the game was "one flat ~768 KB KSEG0 image" loading
contiguously from `0x80000400`. **That is not correct**, and phase 01 exists to
catch exactly this class of error before it reaches generated code.

The true layout is **three separate code segments at three different load
addresses**, copied into RAM during boot:

| Segment | ROM start | ROM end | Size | VRAM | Loaded by |
|---|---|---|---|---|---|
| header | `0x000000` | `0x000040` | `0x40` | — | — |
| IPL3 | `0x000040` | `0x001000` | `0xFC0` | — | — |
| **boot** | `0x001000` | `0x01DCC0` | `0x01CCC0` | `0x80000400` | IPL3 |
| *boot bss* | — | — | `0x8B90` | `0x8001D0C0` | cleared by the entry stub |
| **main** | `0x01DCC0` | `0x0C5BF0` | `0x0A7F30` | `0x80025C50` | routine at `0x80000880` |
| *main bss* | — | — | `0x28920` | `0x800CDB80` | — |
| **aux** | `0x0C5BF0` | `0x0D0A20` | `0x00AE30` | `0x800F64A0` | routine at `0x80000B20` |
| assets | `0x0D0A20` | `0x2000000` | ~31 MB | — | streamed |

Only `boot` maps contiguously from the header's entry point. `main` sits
`0x7F90` further along than a contiguous mapping would put it, and `aux` a
further `0x28920` beyond that — which is why phase 00's contiguity assumption
looked *plausible* (95% of call targets landed inside the assumed window) while
being wrong.

**What survives from phase 00, and matters:** there is still **no overlay
system**. All three segments are copied to fixed addresses during boot, are
never relocated, and are never swapped. That was the load-bearing claim — the
one that decides whether this port needs Beetle Adventure Racing's module
bridge — and it holds. What was wrong was the *number* of segments, not their
nature.

## How the layout was recovered

Three independent methods, which agree. That agreement is the evidence; any one
of them alone would not be.

### 1. The entry stub says where bss is

Disassembling `0x80000400` gives a textbook libultra entry stub: it zeroes
`0x8B90` bytes at `0x8001D0C0`, sets `sp` to `0x80021808`, and jumps to
`0x80000450`. A bss at `0x8001D0C0` is flatly incompatible with a code image
running contiguously to `0x800CF400`, which is what first broke the phase 00
reading.

### 2. Function prologues locate a segment without knowing its loader

`jal` encodes an *absolute* target, so the set of call targets is independent of
any assumption about where code is loaded. A segment's true load address is
therefore the offset `D` that makes the most call targets land on a function
prologue (`addiu sp, sp, -N`) in the ROM.

Scoring candidate offsets that way is decisive:

| Region | Best `D` | Targets on a prologue | Control |
|---|---|---|---|
| boot | `0x7FFFF400` → vram `0x80000400` | 227/380 (59.7%) | ~1% |
| main | `0x80007F90` → vram `0x80025C50` | 1122/2398 (46.8%) | ~1% |
| aux | `0x800308B0` → vram `0x800F64A0` | 107/142 (75.4%) | ~1% |

Two things make this trustworthy. The boot segment solves to **exactly
`0x80000400`**, the value in the cartridge header — a known-true answer the
method was not given, so it is a genuine validation rather than a fit. And the
three offsets partition the address space cleanly: in the `main` bands the
`aux` offset scores 0.7–1.7%, and in the `aux` band the `main` offset scores
0.0%. There is no overlap to argue about.

### 3. The game's own loader confirms it

Both copies go through one routine at `0x800026D8`, with the signature
`(romStart, romEnd, dest, flags)`:

- `0x80000880` calls it with `(0x01DCC0, 0x0C5BF0, 0x80025C50, 0)` → **main**
- `0x80000B20` calls it with `(0x0C5BF0, 0x0D0A20, 0x800F64A0, 0)` → **aux**

These are the same numbers the statistical solve produced, arrived at by
reading the code rather than scoring it. `0x0C5BF0` being both `main`'s end and
`aux`'s start is what ties the two halves together.

### A dead decompression path

The loader has a second branch that would DMA a compressed blob to `0x800F64A0`
and expand it down to `0x80025C50`. It is unreachable: the flag it tests is
built as a literal zero (`lui v0, 0x0` / `addiu v0, v0, 0`), so the `beqz` at
`0x800008E0` always branches past it. Presumably a build-time switch left at
zero for the shipping cartridge.

**This is good news and worth stating plainly: the retail ROM stores its code
uncompressed, and there is no decompressor to reverse.** The dead path is also
where phase 00's spurious "second code segment" call targets came from.

## Splitting and assembling

`recomp/rayman2.us.yaml` is written from scratch — there is no decomp to derive
a config from — and declares the three code segments above plus the asset
remainder. splat 0.32.2 (spimdisasm 1.42.4) then splits **100% of the ROM into
defined segments**, with nothing falling into unknown bins.

**4,444 function labels** were recovered, against phase 00's floor of 2,481.
The gap is the indirect targets that a `jal` scan cannot see, and it is close to
what the `jr $ra` count (3,301) hinted at.

Two mechanical issues stood between the split and a clean link, both solved with
scripts rather than edits to generated files:

- **Unlabelled interior references.** splat emits references such as
  `D_800C8C24` for addresses that land inside another object — the middle of an
  array, a jump-table entry — without emitting a label there. 456 of these.
  Their names encode their own addresses, so `tools/gen_missing_syms.py` reads
  the undefined symbols out of the assembled objects and writes the assignments.
  It also reports any undefined symbol that *doesn't* encode an address, since
  that would be a real problem; there were none.
- **Linker script composition.** splat's `undefined_syms_auto.txt` and
  `undefined_funcs_auto.txt` are assignment fragments. Rather than depend on how
  `ld` merges several `-T` arguments, `scripts/build-elf.sh` concatenates them
  ahead of the generated `SECTIONS` block into one script.

The resulting sections land exactly where the segment map says they should:

```
[ 3] .boot   PROGBITS  80000400  01ccc0  WAX
[ 5] .main   PROGBITS  80025c50  0a7f30  WAX
[ 7] .aux    PROGBITS  800f64a0  00ae30  WAX
```

## What is deliberately not done yet

Two phase 01 items in the plan are naming work, not correctness work. Neither
blocks phase 02 — the recompiler needs boundaries and addresses, which it now
has — but both make everything downstream more readable, and they are carried
forward rather than quietly dropped:

- **Byte-matching libultra against a known SDK build.** Not attempted: it needs
  a reference libultra to match *against*, and no suitable SDK build is
  available here. Until it happens, libultra functions are recompiled like any
  other rather than handed to the runtime through `reimplemented_funcs`. Worth
  revisiting in phase 03, when the runtime makes the payoff concrete.
- **Partitioning by the retained `__FILE__` strings.** The assert anchors
  described in phase 00 are still an unexploited source of module structure.
  This is the cheapest available readability win and should be taken before the
  phase 04 bring-up grind, when navigating 4,444 `func_800xxxxx` names starts
  to cost real time.

## What phase 02 should watch for

- `main`'s bss (`0x800CDB80`, `0x28920`) and `aux`'s (`0x801012D0`) are declared
  `NOBITS` and sized zero in the ELF today. If N64Recomp needs them sized, the
  entry stub only tells us about `boot`'s; the others must be inferred from the
  highest addresses referenced in each segment.
- The `aux` segment ends `0x2A0` before the next ROM data. Whether that tail is
  code, padding or data is not yet established, and `disassemble_all` will have
  guessed.
