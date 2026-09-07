# rayman-2-the-great-escape-recomp

A **native PC port of _Rayman 2: The Great Escape_ (N64, USA)**, built by
**static recompilation** with the [N64Recomp][N64Recomp] toolchain — the same
approach behind [Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp).

> **Status: phase 04 in progress — the game boots clean and idles correctly.**
> Every libultra subsystem the boot path touches is now the runtime's, there are
> no access violations, and the game no longer trips its own assertion: that was
> traced to `OSThreadState` using 0-based values where libultra uses bit flags,
> so the scheduler's `QUEUED` (1) read as `OS_STATE_STOPPED` (1). Fixed in the
> runtime fork. **Nothing renders yet**, so the phase 04 gate is **not** met.
> Details in **[docs/PHASE04-FINDINGS.md](docs/PHASE04-FINDINGS.md)**.
>
> **Phase 03 complete** — RT64 comes up and `recomp_entrypoint` is reached on
> every run.
>
> **Phase 02 complete — the game recompiles.** The ROM splits into an
> assembly-only ELF whose three code segments are **byte-identical** to the
> cartridge (850,464 bytes), and N64Recomp translates all **4,580** functions
> into C that compiles into a **5.2 MB static library** exporting 3,267
> functions. **There is still no runnable executable** — that is phase 03/04,
> where the runtime is wired up. Follow **[docs/PLAN.md](docs/PLAN.md)**;
> findings are in **[docs/PHASE02-FINDINGS.md](docs/PHASE02-FINDINGS.md)**,
> **[PHASE01](docs/PHASE01-FINDINGS.md)** and
> **[PHASE00](docs/PHASE00-FINDINGS.md)**.

> **No game data is included.** You must supply your own legally-dumped USA ROM
> (SHA-1 `50558356b059ad3fbaf5fe95380512b9dceaaf52`). No ROM, asset, or
> ROM-derived file may ever be committed to this repository.

## What this is

Static recompilation translates the N64's MIPS machine code into C
automatically, then links it against a modern runtime — CPU via [librecomp],
graphics via [RT64], OS/audio/input via [ultramodern]. It is **not** a manual
rewrite, and it does **not** require a finished decompilation.

It does require *symbols*: the recompiler needs to know where every function
starts and ends. That is the whole difficulty of this particular game.

## Why this game is different

Most N64 recompilation ports stand on a decompilation project that already
recovered the symbol table. **There is no Rayman 2 N64 decompilation** — no
splat config, no symbol map, no prior port attempt. This project builds its
symbol corpus from the binary.

What the cartridge gives back, measured rather than assumed:

| Aspect | Finding | Why it matters |
|---|---|---|
| **Microcode** | Stock **`F3DEX.NoN 1.23`** — one identifier string in the whole ROM | RT64 already registers this exact GBI, on its F3DEX 1.x path. Not a vendor-custom ucode. |
| **Code layout** | **Three** fixed segments totalling **831 KB**, at `0x80000400`, `0x80025C50` and `0x800F64A0` | **No overlay/module system** — all copied at boot, never relocated. The hardest part of the sibling Beetle port does not exist here. |
| **Saves** | **Controller Pak** only (58 references); no EEPROM/SRAM/Flash | Emulation already exists on the `controller-pak` branch of the runtime fork a sibling port uses. |
| **Functions** | **4,444** recovered by splat (phase 00's `jal` floor was 2,481) | The gap is indirect targets a call scan cannot see. This is the project's real cost. |
| **Symbols** | None public. Asserts kept `__FILE__` (`Actions/Brain.c`, `Culling.c`, …) | No names, but the binary partitions itself into named source modules for free. |
| **Memory** | 4 MB; Expansion Pak optional (hi-res mode) | Flat KSEG0, no TLB-mapped code. |

Every figure above is re-derivable on your own dump:

```bash
python tools/identify_rom.py path/to/rom.z64   # verify the revision
python tools/survey_rom.py   path/to/rom.z64   # reproduce the survey
```

## How it will fit together

```
your ROM ─► splat (asm-only) ─► symbol-rich ELF ─► N64Recomp ─► RecompiledFuncs/*.c ─┐
            [phase 01 done]     [phase 01 done]                                      │
                                                   (recomp/rayman2.us.toml)          │
                                                                                     ├─► CMake ─► exe
patches/*.c ─► clang -target mips ─► patches.elf ─► N64Recomp ─► RecompiledPatches/ ─┤
                                                   (patches.toml)                    │
                        runtime: librecomp + ultramodern + RT64 + RecompFrontend ────┘
```

The ELF is *assembly-only* — symbol names, addresses and sizes, no recovered C
and no matching build. See [docs/PLAN.md](docs/PLAN.md) for why that mode was
chosen over feeding N64Recomp a ROM plus a symbols TOML.

## Repository layout

```
rayman-2-the-great-escape-recomp/
├── docs/
│   ├── PLAN.md                 # the phased build plan and its gates
│   ├── PHASE00-FINDINGS.md     # what the cartridge says, and how it was measured
│   ├── PHASE01-FINDINGS.md     # the segment map, and how the ROM was split
│   ├── PHASE02-FINDINGS.md     # the recompile, and five silent defects
│   ├── PHASE03-FINDINGS.md     # the runtime harness, and where it stops
│   └── PHASE04-FINDINGS.md     # boot bring-up: naming libultra, not emulating it
├── tools/
│   ├── identify_rom.py         # verify a dump is the targeted revision
│   ├── survey_rom.py           # reproduce the phase 00 measurements
│   ├── gen_link_syms.py        # symbol assignments that don't shadow real ones
│   ├── gen_missing_funcs.py    # declare call targets splat missed
│   └── gen_ignored_syms.py     # tell N64Recomp which .text symbols are data
├── scripts/
│   ├── setup-splat.sh          # pinned splat toolchain (WSL / Linux)
│   ├── split-rom.sh            # splat: ROM -> asm/ + linker script
│   ├── build-elf.sh            # assemble + link -> elf/rayman2.us.elf
│   ├── verify-elf.sh           # the phase 01 gate: byte-identity vs the ROM
│   ├── build-recompiler.sh     # build N64Recomp + RSPRecomp from lib/
│   ├── refine-syms.sh          # recover call targets, until it converges
│   ├── recompile.sh            # N64Recomp -> RecompiledFuncs/*.c
│   └── build-recompiled-lib.sh # the phase 02 gate: compile + archive
├── recomp/                     # splat config, linker script, N64Recomp configs
├── src/  include/              # the native host: RT64, input, audio, saves, UI glue
├── patches/                    # C compiled to MIPS that overrides/hooks game functions
├── assets/                     # bundled app assets (no game data, ever)
└── lib/                        # git submodules:
    ├── N64Recomp               # the static recompiler (MIPS -> C)
    ├── N64ModernRuntime        # librecomp (CPU) + ultramodern (OS/audio/input)
    ├── RT64                    # the renderer (D3D12 / Vulkan / Metal)
    └── RecompFrontend          # launcher / settings / input UI (recompui + recompinput)
```

## Building

See **[BUILDING.md](BUILDING.md)** for prerequisites. Through phase 01 the
pipeline splits the ROM and produces the symbol-rich ELF; there is no playable
executable yet.

```bash
git clone --recurse-submodules https://github.com/danielgomesvieira2000/rayman-2-the-great-escape-recomp
cd rayman-2-the-great-escape-recomp
cp /path/to/your/rom.z64 .            # your own dump; never committed
python tools/identify_rom.py rom.z64  # confirm the revision

scripts/setup-splat.sh                # once (WSL or Linux)
scripts/split-rom.sh                  # -> asm/, recomp/rayman2.us.ld
scripts/build-elf.sh                  # -> elf/rayman2.us.elf
scripts/verify-elf.sh                 # byte-identity against your ROM

scripts/build-recompiler.sh           # build the recompiler itself
scripts/recompile.sh                  # -> RecompiledFuncs/*.c
scripts/build-recompiled-lib.sh       # compile it into a static library
```

## Sibling ports

This is the third port in a series, and it reuses their tooling and structure
deliberately:

- **[wave-race-64-recomp](https://github.com/danielgomesvieira2000/wave-race-64-recomp)** —
  the asm-only splat → ELF pipeline this project's phase 01 is modelled on, and
  the display-list rewriter behind its widescreen and interpolation work.
- **[beetle-adventure-racing-recomp](https://github.com/danielgomesvieira2000/beetle-adventure-racing-recomp)** —
  the Controller Pak save emulation this project will consume, and the
  RecompFrontend integration.

## License

**MIT** for this repository's own code — see [LICENSE](LICENSE). A *built
executable* statically links the GPL-3.0 runtime and is therefore distributable
only under the GPL; see **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)** for
the full dependency analysis, including the unresolved licensing status of
RecompFrontend. **Distribute no game assets.**

## Credits

A static-recompilation port is mostly other people's work, and this repository is
a thin layer on top of theirs.

- **Wiseguy** ([@Mr-Wiseguy](https://github.com/Mr-Wiseguy)) and the [N64Recomp]
  contributors — the static recompiler and RSPRecomp, [N64ModernRuntime]
  ([librecomp] and [ultramodern]), and Zelda 64: Recompiled, the project that
  showed the approach works at all.
- **[RT64](https://github.com/rt64/rt64)** contributors — the N64 renderer that
  does the actual drawing, and the GBI database that already knew about this
  game's microcode.
- **[RecompFrontend](https://github.com/N64Recomp/RecompFrontend)** — the
  launcher, settings, input and mod menus, used as-is.
- **The wider N64 decompilation community** — splat, ido-static-recomp,
  asm-differ, objdiff, m2c and decomp-permuter, the shared toolchain any of this
  rests on.
- **Ubisoft Montpellier** — who made the game. This project ports nothing that
  is theirs; it recompiles the code on a cartridge you own.

Built with [Claude Code](https://claude.com/claude-code); commits carry a
`Co-Authored-By` trailer where that is the case.

*If you contributed something credited wrongly or not at all, please open an
issue — it is an oversight, not a claim.*

[N64Recomp]: https://github.com/N64Recomp/N64Recomp
[N64ModernRuntime]: https://github.com/N64Recomp/N64ModernRuntime
[librecomp]: https://github.com/N64Recomp/N64ModernRuntime/tree/main/librecomp
[ultramodern]: https://github.com/N64Recomp/N64ModernRuntime/tree/main/ultramodern
[RT64]: https://github.com/rt64/rt64
