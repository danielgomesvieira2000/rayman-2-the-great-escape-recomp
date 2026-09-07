# Third-party notices

This project's own code (`src/`, `include/`, `patches/`, `tools/`, `recomp/`,
`assets/`) is under the MIT License in [LICENSE](LICENSE). A built executable
also contains the following, under their own terms. License texts are in the
named files inside the submodules under `lib/`.

| Component | Role | License | Text |
|---|---|---|---|
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) (librecomp, ultramodern) | the runtime the recompiled game runs on | **GPL-3.0** | `lib/N64ModernRuntime/COPYING` |
| [N64Recomp](https://github.com/N64Recomp/N64Recomp), RSPRecomp | the recompiler (build-time; its headers are compiled in) | MIT | `lib/N64Recomp/LICENSE` |
| [RT64](https://github.com/rt64/rt64) | the renderer | MIT | `lib/RT64/LICENSE` |
| [RecompFrontend](https://github.com/N64Recomp/RecompFrontend) (recompui, recompinput) | launcher, menus, controller mapping | **no license published** | — |
| [RmlUi](https://github.com/mikke89/RmlUi) | the UI toolkit recompui is built on | MIT | `lib/RecompFrontend/recompui/lib/RmlUi/LICENSE.txt` |
| [lunasvg](https://github.com/sammycage/lunasvg) | SVG icons in the menus | MIT | `lib/RecompFrontend/recompui/lib/lunasvg/LICENSE` |
| GamepadMotionHelpers | gyro handling in recompinput | MIT | `lib/RecompFrontend/lib/GamepadMotionHelpers/LICENSE` |
| SDL2 | window, input, audio | zlib | under `lib/RT64/src/contrib/` |
| DirectX Shader Compiler (`dxcompiler.dll`, `dxil.dll`) | shader compilation | LLVM Release License / Microsoft | `lib/RT64/src/contrib/dxc/` |
| plume, hlsl++, Dear ImGui, nativefiledialog-extended, xxHash, zstd, miniz, o1heap | RT64 and runtime dependencies | MIT / BSD / zlib | under `lib/RT64/src/contrib/` and `lib/N64ModernRuntime/thirdparty/` |
| Lato, Noto Emoji | menu fonts, copied from RmlUi's samples at build time | SIL Open Font License 1.1 | `lib/RecompFrontend/recompui/lib/RmlUi/Samples/assets/LICENSE.txt` |

## What this means for a built executable

The runtime is GPL-3.0 and is linked statically, so **any executable built from
this project is a combined work distributable only under the GNU General Public
License, version 3**, whatever the license of this project's own files. Anyone
distributing such a binary must make the complete corresponding source
available under the GPL's terms; this repository, at the commit the binary was
built from, together with the submodules it pins, is that source.

The MIT license on this repository's own files is not in conflict with that. It
governs the files here — which anyone may reuse under MIT terms — while the
*combined binary* inherits the GPL from the runtime it links.

## RecompFrontend

RecompFrontend publishes no license. Without one, no permission to copy or
redistribute it exists beyond what its authors grant. This project consumes it
as a submodule, as the other N64: Recompiled projects do, and does not vendor a
copy. **Anyone intending to distribute a built executable should resolve this
with the RecompFrontend authors first.**

## The game

No Ubisoft code or data is present in this repository, and none will be. The
recompiled game code is generated on the builder's machine from the builder's
own cartridge dump and is never committed; a built executable contains that
generated code and loads the game's assets from the player's own dump at run
time.

The measurements in `docs/PHASE00-FINDINGS.md` — hashes, addresses, sizes, the
microcode identifier and the retained source-file names — are facts *about* the
binary, established by the tools in `tools/`, and reproduce none of it.

## Prior art relied on

No Rayman 2 N64 decompilation exists, so unlike the sibling ports this project
derives no symbol data from any third-party reverse-engineering project. Its
symbol corpus is built from the cartridge by the tooling in this repository.
Should that change — should a decomp appear, or symbols be contributed — its
provenance and license will be recorded here before anything is consumed.
