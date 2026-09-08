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

### 03 — Runtime harness ✅

Register RT64. Wire VI timing. Controller callback into `OSContPad`. Audio
callback through ultramodern. Controller Pak saves — start from the
`controller-pak` branch of the N64ModernRuntime fork, do not rewrite `osPfs*`.
ROM ingest on first run, into a per-user data directory.

**Gate:** the executable reaches `recomp_entrypoint` and runs the first thread.
*Met:* `recomp_entrypoint` is reached and the game thread runs, on 10 of 10
runs. The port brings RT64 up itself (`src/rt64_context.cpp`) rather than
depending on the frontend. Two bugs stood in the way, both in the hand-written
game entry rather than anywhere exotic: the entrypoint address was not
sign-extended, so the emulated IPL3 DMA wrote 4 GB past RDRAM; and the port
never called `load_stored_rom()`, so that DMA had an empty source span. See
[PHASE03-FINDINGS.md](PHASE03-FINDINGS.md).

**Amendment.** This phase said "Register RT64" and left RecompFrontend to phase
06. That ordering is wrong: `create_render_context` is mandatory, has no
default, and the only implementation in the tree is the frontend's, because
RecompFrontend owns the RT64 application its overlay menus hook into. The
frontend is therefore a phase 03 *dependency*, and every failure so far has been
in its bring-up rather than in the game path. The alternative — writing a
minimal RT64 context of our own — is more code but separates "does the
recompiled game run" from "does the menu system initialise", which the gate
implies should be separable and currently are not.

### 04 — Boot bring-up

Where ports of this kind get stuck, and where the schedule risk lives. Expect to
land on librecomp's `osEPiRawStartDma` guard repeatedly. Instrument with a
function-entry trace from the start: a debugger over ~20 MB of generated C is
not a plan, and the sibling ports' `instrument_funcs.py` exists for this.

**Gate:** the Ubisoft logo, then the attract sequence, rendering recognisably.
*Met.* The port boots, shows the game's Controller Pak prompt, takes a button
press and plays the intro cinematic for forty seconds at a sustained 58-61
display lists a second with no crashes.

Almost every fault was the same one wearing different clothes: a libultra entry
point left unnamed, so the game drove hardware the runtime does not model. The
exception took the longest and is a property of the execution model rather than
of any symbol -- ultramodern's scheduler cannot preempt, so a game thread that
busy-waits stops the whole program, including delivery of the very event it is
waiting for. tools/find_spin_loops.py finds that class now. See
[PHASE04-FINDINGS.md](PHASE04-FINDINGS.md).

### 05 — Graphics and audio correctness

Confirm RT64 dispatches `F3DEX.NoN 1.23`, and clear the "Needs confirmation"
annotation on that GBI entry — with evidence, upstream if it holds. This phase
carries the project's main unpriced risk: F3DEX 1.x command-level defects that
no F3DEX2 title would ever expose. Verify the fog and the transparent water,
which are the game's signature effects and the usual casualties under HLE
renderers. Audio through ultramodern.

**Gate:** the first three levels play start to finish with correct visuals,
audio and Controller Pak saves. *Not met.* The graphics microcode is confirmed
and the annotation cleared, audio works through a recompiled copy of the game's
own microcode, and Controller Pak saving is done — but nobody has played three
levels, so the part of the gate that is actually about the game is untested. See
[PHASE05-FINDINGS.md](PHASE05-FINDINGS.md) and
[CONTROLLER-PAK-FINDINGS.md](CONTROLLER-PAK-FINDINGS.md).

**Phase 06 was entered anyway, and that is a departure from the rule at the top
of this section.** It was taken deliberately rather than drifted into: the
remainder of this gate is a playtesting result, playtesting needs the launcher
and rebindable controls that phase 06 carries, and there was no way to reach the
one through anything but the other. The cost is that the enhancement phase was
built on an unfinished correctness phase, and phase 07 opens by paying that
back.

### 06 — Enhancements and release ✅

Widescreen and arbitrary internal resolution through RT64.
**[RecompFrontend](https://github.com/N64Recomp/RecompFrontend)** for the
launcher, settings, input rebinding and pause menu — the same frontend as the
two sibling ports, consumed as a submodule and not forked. High frame rate via
RT64 interpolation between game frames, leaving the game's own update rate
alone. CI that builds without a ROM. A first-run flow that explains the ROM
requirement to someone who has read none of this.

**Gate:** a stranger with a dump and no context can build it and play it.
*Met on the play half, and only documented on the build half.* `v0.1.0-alpha`
is published with a Windows x64 archive, so a stranger with a dump does not have
to build anything; `BUILDING.md` describes the pipeline but nobody outside this
machine has run it, which is what phase 07's CI item is for.

Two things did not go to plan, and both are worth recording rather than
quietly absorbing.

The frontend arrived early. Phase 03's amendment had already established it as a
dependency rather than an enhancement, and it was then brought forward again
ahead of phase 05's gate for a plainer reason: playtesting needs a launcher and
rebindable controls, and the alternative was driving the game with a hardcoded
keyboard map. See [FRONTEND-FINDINGS.md](FRONTEND-FINDINGS.md).

**High frame rate is implemented, understood, and off.** RT64's interpolation
needs no per-game work — it matches transforms automatically when a ROM does not
tag them — and the physics question is settled by measurement rather than
argument: the extra frames are generated on the renderer's side of the display
list, and the game's own audio production rate is unchanged in every
configuration. What blocks it is that the presentation mode it requires
(`SkipBuffering`, or `PresentEarly`) presents the framebuffer the game has just
drawn, which means presenting one the game may still be drawing into. On this
game that is visible: whole regions of the scene missing, HUD digits sliced off.
It ships as `Console` and stays reachable through `RAYMAN2_PRESENT`. See
[HIGH-FRAMERATE.md](HIGH-FRAMERATE.md).

Delivered beyond the original list: Controller Pak saving emulated at the joybus
level ([CONTROLLER-PAK-FINDINGS.md](CONTROLLER-PAK-FINDINGS.md)), and per-session
debug reports written for a playtester to send and an assistant to read
([DEBUG-REPORTS.md](DEBUG-REPORTS.md)).

### 07 — Coverage and hardening

The old phase 07 said it would be written at the 06 gate, from defects observed
in motion, because the sibling ports each found their real work only after
playing the thing. That is where this project now is, with one difference worth
stating plainly at the top:

**This port has been tested but barely played.** Every claim in `docs/` rests on
instrumented runs of a few minutes, mostly automated, mostly in the first level.
Phase 05's gate — the first three levels start to finish — has not been met by a
human at the controls. Almost everything below is downstream of fixing that.

The sequence matters more than the list. Bug-fixing without a regression net is
how ports of this kind quietly rot: a fix for level seven breaks level two and
nobody learns until a player says so.

**First, make regressions detectable.** Roughly an afternoon each, and all of it
pays for itself the first time something breaks:

- A scripted smoke run: boot, load a save, render a fixed number of frames, hash
  the framebuffer, and assert the session report contains no ERROR lines. It
  catches "the port stopped booting" and "the picture changed" with nobody
  watching.
- CI that builds without a ROM. This was on the phase 06 list and did not get
  done. It is what stops "works on this machine" from becoming a category of
  bug, and it is the missing half of the 06 gate.
- A library of Controller Pak images parked at known points, so reaching a level
  costs seconds rather than an hour of replay.

**Then play it, all of it, and let the reports decide the rest.** This is the
only instrument that finds the risk the plan has named from the start —
F3DEX 1.x command-level defects, which are invisible to code reading and to any
test written in advance, and which surface as one wrong-looking wall in a level
nobody has reached. Finding out early is what protects the decision recorded
under "What would make this project stop".

**Alongside that, close the named graphics risks deliberately.** Fog and
transparent water are this game's signature effects and phase 00 flagged them as
the usual casualties under an HLE renderer. They are checkable at known
locations in a single session, and the answer decides whether phase 05's gate is
real rather than assumed.

**Known open items**, none of which should be started before the two steps
above:

- Frame interpolation tears (above). The 144 Hz measurement has not been taken;
  the fix may belong upstream in RT64 rather than here.
- Controller Pak coverage is create-and-read only. Deleting a save, a second
  slot, a full pak and a corrupted pak are all untested paths through the
  cartridge's own filesystem.
- The Rumble Pak is given up for the Controller Pak, as it would be on a console
  with one accessory slot. Serving both at once from the joybus layer is
  possible — their address ranges do not overlap — and is a deliberate deviation
  from hardware if taken.
- Audio *rate* is verified; audio *content* is not. Music, effects and cutscene
  synchronisation across the game are unexamined.
- Windows only. Linux and macOS are real work, not a checkbox.
- Performance headroom is unknown on anything but one Intel Iris Xe laptop.
- librecomp's mod system is wired and never exercised.

**Gate (1.0):** the game completes start to finish on a fresh install, driven by
someone who has read none of this, with no crash and no visual defect a player
would think worth reporting — on at least two machines with different GPU
vendors.

The two-vendor clause is not padding. An Intel-only sample is how renderer bugs
reach users, and this project's largest unpriced risk lives in the renderer.

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
