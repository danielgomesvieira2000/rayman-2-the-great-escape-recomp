# Phase 00 findings — what the cartridge says

Everything here was measured from a dump with `tools/identify_rom.py` and
`tools/survey_rom.py`, both of which are in this repository so the numbers can
be re-derived rather than trusted. No ROM data is reproduced here — only
measurements of it.

## The revision this port targets

| Field | Value |
|---|---|
| Title | `Rayman 2` |
| Cartridge | **NUS-NY2E** (USA), version 0 |
| Size | 33,554,432 bytes (32 MB) |
| Entry point | `0x80000400` |
| Header CRC | `0xF3C5BF9B` `0x160F33E2` |
| SHA-1 | `50558356b059ad3fbaf5fe95380512b9dceaaf52` |
| MD5 | `03aa4d09fde77eed9b95be68e603d233` |

`tools/identify_rom.py` accepts `.z64`, `.v64` and `.n64` dumps, normalises to
big-endian before hashing, and exits non-zero on anything that is not the above.

## 1. The graphics microcode — the single most important finding

The ROM carries exactly **one** microcode identifier string:

```
RSP Gfx ucode F3DEX.NoN 1.23 Yoshitaka Yasumoto Nintendo.
```

So the game uses **stock F3DEX 1.23 with near-clipping disabled** — a standard
SDK microcode, *not* a vendor-custom one, and *not* F3DEX2.

This matters because it decides whether RT64 can draw the game at all. RT64
keeps a database of known GBI versions in `src/gbi/rt64_gbi.cpp`, and this exact
microcode is already registered there:

```
F3DEX_NON_1_23 = { "F3DEX.NoN 1.23", GBIUCode::F3DEX, { LowP:false, NoN:true, ... } }
```

It routes to RT64's F3DEX (1.x) command table with the no-near-clip flag set.
The entry is annotated "Needs confirmation" upstream, meaning it is registered
but not yet verified against a shipping game — neighbouring entries on the same
code path (`F3DEX 1.21`, `F3DEX 1.23`, `F3DEX.NoN 1.22`) carry no such note and
are confirmed.

**Read this as: the renderer path exists and is the right one, but this port is
plausibly the thing that confirms it.** Budget for F3DEX 1.x command-level bugs
in RT64 that F3DEX2 titles never hit. That is a real risk and it is priced into
phase 05, not waved away.

There is a useful precedent in the sibling ports: Wave Race 64 needed a
*game-specific* microcode entry (`GBIUCode::F3DWAVE`) and still worked out. A
stock SDK microcode is a materially easier starting position than that.

## 2. Saves — Controller Pak, not EEPROM

58 references to the Controller Pak appear in the ROM, including the user-facing
failure message the game shows when none is present. There is no EEPROM, SRAM or
Flash save path.

This is a **direct reuse win**: Beetle Adventure Racing has the same save
medium, and the Controller Pak emulation written for it already lives on the
`controller-pak` branch of the N64ModernRuntime fork that port consumes. This
project should start from that branch rather than re-implement `osPfs*`.

## 3. The code is one flat image — no module system

> **Superseded by [PHASE01-FINDINGS.md](PHASE01-FINDINGS.md).** The
> no-overlay-system conclusion below holds, and it was the load-bearing one.
> The "one flat image" claim does not: the game has **three** code segments,
> copied to three fixed addresses during boot (`0x80000400`, `0x80025C50`,
> `0x800F64A0`). Only the first maps contiguously from the header's entry
> point, which is why the contiguity assumption below looked plausible while
> being wrong. The section is left as written; phase 01 records the correction
> and how it was found.

A 4 KB-block scan classifying blocks as code (>90% decodable MIPS III, ≥4 call
or return instructions) finds the game's code in one contiguous span:

| ROM range | VRAM | Size |
|---|---|---|
| `0x001000`–`0x019000` | `0x80000400` | 96 KB |
| `0x01E000`–`0x093000` | `0x8001D400` | 468 KB |
| `0x096000`–`0x0BF000` | `0x80095400` | 164 KB |
| `0x0C6000`–`0x0D0000` | `0x800C5400` | 40 KB |

That is **~768 KB of code between ROM `0x1000` and `0x0D0000`**, loading
contiguously from `0x80000400`. The gaps are embedded data — the microcode blob
itself sits at `0x01CD10`, inside the first gap.

The scattered 8–16 KB blocks the tool reports past `0x0A64000` are **false
positives in asset data**, not overlays. Two independent checks say so: their
entropy profile matches the surrounding compressed data, and the out-of-range
call targets that would imply a second code segment all originate from just 27
blocks that are themselves the known data regions (the microcode blob and the
tail tables). Decoding the ROM at the offset those targets would imply yields
high-entropy noise, not instructions.

**The consequence for the plan is large.** Beetle Adventure Racing needed an
overlay bridge for ~130 relocatable modules, and that was the hardest part of
that port. Rayman 2 appears to need none of it: one flat KSEG0 image, the Wave
Race shape rather than the Beetle shape. This assumption is load-bearing and
phase 01 is where it gets confirmed or killed.

## 4. Function count — a floor of 2,481

Scanning the flat image for `jal` targets:

- 9,985 `jal` instructions
- 2,667 unique targets, **2,481 of them inside the code window** (96.0% of calls)
- 3,301 `jr $ra` instructions

So **at least 2,481 distinct functions need symbols**, and the ~3,301 returns
suggest the true count is higher — functions reached only indirectly (through
jump tables and function pointers) never appear as a `jal` target. This is a
floor, not an estimate.

For scale: Wave Race 64's equivalent pass took a 756-function starting set to
1,228. Rayman 2 is roughly twice that game, with no starting set at all.

## 5. There is no Rayman 2 N64 decompilation

This is the defining constraint of the project, and it is worth being blunt
about. Searches across GitHub and the decompilation community turn up no N64
Rayman 2 decomp, disassembly, splat configuration or symbol map. The Rayman 2
projects that do exist target a different codebase entirely — the PC version's
CPA engine (`OpenRayman`, the modding tools) — and none of it carries N64 MIPS
symbols.

Both sibling ports had a donor: Beetle consumed BeetleDecomp; Wave Race ported
LLONSIT's Rev A splat config across revisions. **This project has neither.** The
symbol corpus has to be built from the binary. That is the single biggest cost
in the plan, and phase 01 exists entirely to pay it.

## 6. What the binary gives back — retained source-file names

The game's asserts were compiled in with `__FILE__` intact, so the ROM contains
the original source-tree paths:

```
Actions/3dData.c     Actions/Brain.c      Actions/CollSet.c    Actions/Dynam.c
Actions/MSlight.c    Actions/MSSound.c    Actions/MSWay.c      Actions/MS_Micro.c
Actions/SectInfo.c   Actions/StdGame.c    Actions/cineinfo.c   PlayAnim/PlayAnim.c
Culling.c            HieMtStk.c           ../../Public/Geo/GeoBdVol.h
```

Together with French-language debug strings (`AllocTmp: ecrase les données`,
`Manque %d octets`), this identifies the codebase as Ubisoft's in-house engine
lineage, and hands the project something genuinely valuable: **each assert site
localises the function containing it to a named source file.** Every such site
is a free, reliable name hint anchored to a real address. It will not name
functions, but it partitions a large fraction of them into modules, which is
most of the work of making 2,481 symbols navigable instead of a wall of
`func_800xxxxx`.

## What phase 01 must settle

1. Is the flat-image finding right — does anything relocate or load code at run time?
2. How many functions are there really, once indirect targets are recovered?
3. Do the libultra functions byte-match a known SDK build, so they can be handed
   to the runtime as `reimplemented_funcs` instead of being recompiled?
