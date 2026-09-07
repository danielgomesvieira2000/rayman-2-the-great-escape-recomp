# Build plan

## Target revision

This project targets **Rayman 2 (USA), NUS-NY2E, version 0** — entry point
`0x80000400`, header CRC `0xF3C5BF9B 0x160F33E2`, SHA-1
`50558356b059ad3fbaf5fe95380512b9dceaaf52`.

One revision, chosen and pinned. PAL and the later Japanese release are out of
scope until the USA build plays; a second revision multiplies the symbol work,
which is the expensive part here, and buys nothing until there is something to
port across.

## The problem this project actually has

Read [PHASE00-FINDINGS.md](PHASE00-FINDINGS.md) first; the short version is that
the cartridge is unusually friendly and the surrounding ecosystem is unusually
bare.

**Friendly:** the game uses a stock SDK microcode (`F3DEX.NoN 1.23`) that RT64
already has a table entry for. Its 831 KB of code sits in three segments copied
to fixed addresses at boot, with no module or overlay system and no relocation
— the hardest single part of the Beetle Adventure Racing port simply does not
exist here. (Phase 00 read this as one flat image; phase 01 corrected the
segment count while confirming the no-overlay claim.) It saves to a Controller Pak, for which
working emulation already exists in a sibling port. Its asserts kept `__FILE__`,
so the binary tells you which source file most functions came from.

**Bare:** there is no Rayman 2 N64 decompilation. None. No splat config, no
symbol map, no disassembly, no prior recompilation attempt. Both sibling ports
started from a donor project; this one starts from a 32 MB binary and a header.

So the risk here is not architectural. It is **volume**: the 4,444 functions
phase 01 recovered have to be given names and — the part that actually bites —
verified boundaries, with nothing to check against but the binary itself.

## The decision this project is built on

N64Recomp accepts exactly one of two input modes: an ELF (`elf_path`), or a ROM
plus a symbols TOML (`symbols_file_path` + `rom_file_path`).

Symbols-file mode reaches a first result sooner and needs no MIPS toolchain. It
is still the wrong choice, for the same reason it was wrong for Wave Race 64:
upstream rejects `func_reference_syms_file` and `data_reference_syms_files`
outside ELF mode, which forfeits the reference-symbol workflow and the
single-file patch output — and those are what make the `patches/` layer, and
therefore every enhancement in phases 06 and 07, iterate in seconds instead of
minutes.

**So: we assemble our own ELF.** Not a decompilation — an *assembly-only* ELF
built from splat output. It needs symbol names, addresses and sizes; it needs no
recovered C and no matching build. This is the Wave Race 64 pipeline, and it is
already proven on a game whose donor symbols were for the wrong revision.

Sources of symbols, in descending order of trust:

1. **Splat's own analysis** of the three code segments — function boundaries
   recovered from control flow. This delivered 4,444 functions in phase 01,
   which is where the majority came from.
2. **A JAL-target scan** for call targets splat does not classify as functions.
   The phase 00 floor was 2,481; `tools/survey_rom.py` performs this pass.
3. **Byte-matching libultra against a known SDK build**, so those functions can
   be renamed and handed to the runtime through `reimplemented_funcs` rather
   than recompiled at all. On a 1999 title this should account for a meaningful
   slice of the 2,481 and is the cheapest win available.
4. **The retained `__FILE__` strings**, to partition what remains into named
   modules. Not names, but structure — and structure is what makes the rest
   tractable by hand.

Disagreements between these sources are bugs, not noise. A wrong function
boundary produces C that compiles cleanly and then corrupts state at run time,
which is the most expensive class of defect this project can create.

## Phases

Each gate is the entry condition for the next phase. No phase is entered through
anything but its predecessor's gate.

### 00 — Ground rules and skeleton ✅

Repository, submodules, a `.gitignore` that refuses game data, ROM
identification and survey tooling, and the findings above.

**Gate:** the tree configures and builds; `tools/identify_rom.py` accepts a
correct dump and rejects everything else. *Met.*

### 01 — Split the ROM ✅

The long, unglamorous phase, and the one that decides whether this project is
viable. Stand up splat against the flat image. Recover function boundaries; run
the JAL scan; byte-match and rename libultra; partition the remainder using the
`__FILE__` anchors. Confirm — or kill — the no-overlay finding by auditing every
DMA into RDRAM for anything that lands in an executable range.

**Gate:** an assembled ELF whose `.text` is byte-identical to the ROM's code.
Byte-identity is the whole point: it is the only check that proves the symbol
table describes the binary rather than a plausible fiction. *Met:* 850,464
bytes across three sections, 4,444 functions recovered. The audit killed phase
00's "one flat image" reading — there are three fixed-address segments — while
confirming the claim that mattered: no overlay system. Two naming sub-tasks
(libultra byte-matching, `__FILE__` partitioning) are carried into later phases;
see [PHASE01-FINDINGS.md](PHASE01-FINDINGS.md).

### 02 — First recompile ✅

Write `recomp/rayman2.us.toml`, run N64Recomp, compile the output. Script the
fixes for writes to `$zero` and for unresolved jump tables. Keep
`--dump-context` output in tree as the symbol reference the `patches/` layer
links against.

**Gate:** every generated `funcs_*.c` compiles and links into a static library.
*Met:* 4,580 functions translated with no errors; 66 objects archived into a
5.2 MB library exporting 3,267 functions. Every defect found in this phase was
silent -- including one that emitted 1 function out of 4,465 and exited
successfully -- so each pipeline stage now reports counts. See
[PHASE02-FINDINGS.md](PHASE02-FINDINGS.md).

### 03 — Runtime harness

Register RT64. Wire VI timing. Controller callback into `OSContPad`. Audio
callback through ultramodern. Controller Pak saves — start from the
`controller-pak` branch of the N64ModernRuntime fork, do not rewrite `osPfs*`.
ROM ingest on first run, into a per-user data directory.

**Gate:** the executable reaches `recomp_entrypoint` and runs the first thread.

### 04 — Boot bring-up

Where ports of this kind get stuck, and where the schedule risk lives. Expect to
land on librecomp's `osEPiRawStartDma` guard repeatedly. Instrument with a
function-entry trace from the start: a debugger over ~20 MB of generated C is
not a plan, and the sibling ports' `instrument_funcs.py` exists for this.

**Gate:** the Ubisoft logo, then the attract sequence, rendering recognisably.

### 05 — Graphics and audio correctness

Confirm RT64 dispatches `F3DEX.NoN 1.23`, and clear the "Needs confirmation"
annotation on that GBI entry — with evidence, upstream if it holds. This phase
carries the project's main unpriced risk: F3DEX 1.x command-level defects that
no F3DEX2 title would ever expose. Verify the fog and the transparent water,
which are the game's signature effects and the usual casualties under HLE
renderers. Audio through ultramodern.

**Gate:** the first three levels play start to finish with correct visuals,
audio and Controller Pak saves.

### 06 — Enhancements and release

Widescreen and arbitrary internal resolution through RT64.
**[RecompFrontend](https://github.com/N64Recomp/RecompFrontend)** for the
launcher, settings, input rebinding and pause menu — the same frontend as the
two sibling ports, consumed as a submodule and not forked. High frame rate via
RT64 interpolation between game frames, leaving the game's own update rate
alone. CI that builds without a ROM. A first-run flow that explains the ROM
requirement to someone who has read none of this.

**Gate:** a stranger with a dump and no context can build it and play it.

### 07 — Beyond parity

Deliberately not specified yet. The sibling ports each found their real
enhancement work only after playing the thing — Wave Race's widescreen 2D
anchoring and its vertex interpolation were not foreseeable from a plan. This
section gets written at the 06 gate, from defects observed in motion.

## What would make this project stop

Stated in advance, so the decision is not made under sunk cost:

- **Phase 01 cannot reach a byte-identical `.text`.** If the flat-image finding
  is wrong and code is relocated at run time in a format the tooling cannot
  read, the cost changes shape entirely and the plan needs rewriting, not
  pushing through.
- **RT64's F3DEX 1.x path is substantially broken.** Recoverable — it is fixable
  code and the fix belongs upstream — but it converts this from a port project
  into a renderer project, which is a different undertaking and should be
  entered deliberately.

## Standing constraints

- **No ROM, asset, or ROM-derived file is ever committed.** The builder supplies
  their own dump; every generated artefact is produced on their machine.
- **Generated code is never hand-edited.** If the output is wrong, fix the
  config or write a script in `tools/`. A hand-edit is lost at the next
  regeneration, and lies about its provenance until then.
- **The frontend is consumed, not forked.** RecompFrontend is a submodule.
  Changes that belong upstream go upstream.
- **Findings are written down as they are made**, including the negative
  results — those are the expensive ones and the easiest to lose.
- **Measurements beat recollection.** Every claim in `docs/` should be
  re-derivable from a tool in `tools/`, or it does not belong there.
