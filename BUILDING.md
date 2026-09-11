# Building rayman-2-the-great-escape-recomp

> **Phase 03.** The full pipeline runs and produces an executable that starts,
> brings up RT64 and then crashes on the renderer thread; the game does not
> play yet. Everything below works as written. The phase gates are in
> [docs/PLAN.md](docs/PLAN.md).

## What you need first

A **legally-dumped USA cartridge** of Rayman 2: The Great Escape. Nothing here
ships game data, and nothing here will help you obtain it.

Verify it before anything else:

```bash
python tools/identify_rom.py path/to/rom.z64
```

It accepts `.z64`, `.v64` and `.n64` dumps, normalises to big-endian, and exits
non-zero unless the dump is SHA-1
`50558356b059ad3fbaf5fe95380512b9dceaaf52` (NUS-NY2E, version 0). Other
revisions and regions are not supported; see [docs/PLAN.md](docs/PLAN.md).

To reproduce the measurements the plan rests on:

```bash
python tools/survey_rom.py path/to/rom.z64
```

Both tools need only Python 3.9+ and the standard library.

## Two toolchains, which are not the same thing

Don't conflate them — the sibling ports both lost time to this:

- **The host app** (runtime + RT64 + the recompiled C) → **clang-cl** on
  Windows, **clang** on Linux and macOS, via CMake + Ninja.
- **The MIPS patches** (phase 06 onward) → cross-compiled with
  `clang -target mips` + `ld.lld`, **pinned to LLVM 18.1.8**. LLVM 19.x
  miscompiles MIPS, and Apple Clang cannot target MIPS at all.

A third toolchain appears in phase 01: **splat**, run under Linux or WSL, to
split the ROM and assemble the symbol-rich ELF. Only the recomp builds natively
on Windows.

Cross-platform by design (RT64: D3D12 on Windows, Vulkan on Linux, Metal on
macOS). Windows is the primary target.

## Prerequisites — Windows

No full Visual Studio IDE is needed; **CLion** plus the **VS Build Tools** is
enough.

1. **Build Tools for Visual Studio 2022** with the **Desktop development with
   C++** workload (MSVC v143 + a Windows 10/11 SDK) and the **C++ Clang tools
   for Windows** component, which provides `clang-cl`:

   ```
   winget install Microsoft.VisualStudio.2022.BuildTools --override ^
     "--quiet --add Microsoft.VisualStudio.Workload.VCTools ^
      --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --includeRecommended"
   ```

   Or take `clang-cl` from a standalone LLVM (`winget install LLVM.LLVM`, any
   LLVM 17–19).
2. **CMake ≥ 3.20 and Ninja** — CLion bundles both.
3. **WSL** with a Ubuntu distribution, for splat and the ELF assembly.

RT64's D3D12 backend needs only the Windows SDK and RT64's bundled DXC
(`lib/RT64/src/contrib/dxc`) — **no** DirectX Agility SDK. SDL2 is fetched
automatically, and the runtime DLLs are copied next to the executable by the
build.

## Prerequisites — Linux

```bash
sudo apt-get install cmake ninja-build libsdl2-dev libgtk-3-dev \
                     libfreetype-dev lld llvm clang python3-venv
```

Build with **clang**; GCC fails the final link on recompiled-symbol collisions.
RT64 uses Vulkan here.

## Prerequisites — macOS

Native Metal via RT64. Needs Homebrew LLVM plus CMake and Ninja. Apple Clang
builds the host app but **cannot** build the MIPS patches — use Homebrew LLVM
18.x for those.

## The pipeline

```bash
# 1. Dependencies, and the recompiler itself.
git submodule update --init --recursive
python tools/patch_rt64_debug_menu.py # the F1 debug menu's hook into RT64
scripts/setup-splat.sh                # splat, in WSL or on Linux
scripts/build-recompiler.sh           # -> ./N64Recomp, ./RSPRecomp

# 2. Split the ROM and assemble the symbol-rich ELF (WSL / Linux).
scripts/split-rom.sh                  # splat -> asm/ + recomp/rayman2.us.ld
scripts/build-elf.sh                  # assemble + link -> elf/rayman2.us.elf
scripts/verify-elf.sh                 # byte-identity against your ROM

# 3. Recompile the game to C.
scripts/recompile.sh                  # -> RecompiledFuncs/*.c, and a manifest

# 4. Configure and build the port (from an x64 Native Tools prompt).
cmake -S . -B build-cmake -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j
```

Re-run step 3 whenever the ELF or `recomp/rayman2.us.toml` changes, and steps
2-3 whenever `recomp/symbol_addrs.txt` or the splat config changes.

**Re-run `tools/patch_rt64_debug_menu.py` after any `git submodule update`.** It
adds the one function pointer RT64 calls to let the port draw its own window
inside the developer UI (`docs/DEBUG-MENU.md`), and it also unbinds F2, which
upstream uses to toggle ray tracing for the whole session with nothing on screen
to say so. `lib/RT64` is a submodule, so an update silently reverts both, and the
port *links* against the symbol whether or not the menu is ever opened — a fresh
clone that skipped this fails at link time rather than at runtime. The script is
idempotent and safe to run at any point.

**Two build configurations.** `-DRAYMAN2_ENABLE_FRONTEND=ON` (the default)
builds with RecompFrontend's launcher and uses its RT64 context.
`-DRAYMAN2_ENABLE_FRONTEND=OFF` builds without any UI and uses the port's own
context in `src/rt64_context.cpp`, which is what makes the game bootable
independently of whether the menu system initialises.


## Checking that a build is reproducible

`scripts/recompile.sh` records a manifest of every input it consumed and every
artifact it produced (`recomp/pipeline.manifest.json`, committed). A later run
can check itself against it:

```bash
python tools/manifest.py check
```

It exits non-zero, loudly, if the outputs moved while the inputs did not. That
is the case worth catching: it means re-running the pipeline is not giving the
same result, and no build can then be reasoned about by comparing it with a
previous one.

Measured as of this writing, the pipeline **is** reproducible -- splitting,
assembling and recompiling twice from the same ROM and the same committed
configuration produces byte-identical `asm/`, ELF and `RecompiledFuncs/`.

## Notes

- Keep the tree LF-normalised (`.gitattributes`) even on Windows.
- **Never commit a ROM, an ELF, or anything derived from them.** `.gitignore`
  is written to refuse them, but it is a safety net, not the rule.
- **Never hand-edit generated code.** Fix the config, or add a script under
  `tools/`; a hand-edit is lost at the next regeneration.
