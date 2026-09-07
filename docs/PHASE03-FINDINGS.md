# Phase 03 findings — runtime harness

**Gate NOT met.** The port builds and links into `rayman2-recomp.exe`, RT64
initialises Direct3D 12 and enumerates the GPU, and then the process dies on the
renderer thread before the game thread starts. `recomp_entrypoint` has not been
reached.

What exists and works:

| Piece | State |
|---|---|
| CMake build (clang-cl + Ninja, Windows) | **Builds clean**, 212 targets |
| Recompiled game code → static library | **Links** into the executable |
| RT64 / D3D12 | **Initialises**; reports device, vendor, driver version |
| librecomp + ultramodern | Linked; `recomp::start()` entered |
| Section registration | Wired (`src/register_sections.cpp`) |
| The three port-owed libultra routines | Implemented (`src/port_runtime.cpp`) |
| SDL window, audio, input, Controller Pak reporting | Written, not yet exercised |
| Reaching `recomp_entrypoint` | **No** |

## Where it stops

```
[rayman2] start
[rayman2] entering recomp::start
Device Name: Intel(R) Iris(R) Xe Graphics
Device Vendor: 0x8086
Driver Version: 0x1f000000651442
<exit 0xC0000409>
```

`0xC0000409` is a Windows fail-fast. `main()` wraps `recomp::start()` in a
`try`/`catch` that prints what it catches, and it prints nothing — so the throw
is on **another thread**, the renderer thread, where it crosses a `noexcept`
boundary and terminates the process instead of unwinding.

Two causes of exactly this were found and fixed, and the pattern is worth
recording because the failure signature is identical every time and carries no
information at all:

1. **No primary font registered.** recompui throws
   `"No primary font was registered"` while building its menus. Fixed by
   registering LatoLatin and staging the fonts to `assets/`.
2. **No `assets/recomp.rcss`.** recompui loads the port's stylesheet by name in
   `init_styling` and throws if it is absent. Fixed by writing one.

A third throw of the same shape remains. The next step is to find it rather
than guess: build RecompFrontend with exceptions surfaced, or attach a debugger
and break on throw, instead of continuing to fix candidates one at a time.

## The plan was wrong about when the frontend arrives

The build plan puts RecompFrontend in **phase 06** ("launcher, settings, input
rebinding") and phase 03 says only "Register RT64". That ordering does not
survive contact with the code: **the render context is the frontend's**.

`ultramodern::renderer::callbacks_t::create_render_context` is mandatory, has no
default, and the only implementation available is
`recompui::renderer::create_render_context` — RecompFrontend owns the RT64
application because its menus draw as an overlay on top of the game and it needs
the render hooks. A port either uses the frontend's context or writes its own
~400-line RT64 context (the sibling Beetle port has both and switches between
them).

So the frontend is not a phase 06 nicety; it is a phase 03 dependency, and every
remaining failure is in its bring-up rather than in the game path. **The plan
should be amended rather than worked around**, and the honest options are:

- Carry on with the frontend as a phase 03 dependency, which is what the code
  wants and what this repository now does; or
- Write a minimal RT64 context of our own so the game can be brought up
  headless, and adopt the frontend later as the plan intended. This is more
  code, but it separates "does the recompiled game run" from "does the menu
  system initialise", which are independent questions currently entangled.

The second is probably the better engineering, precisely because the gate —
reaching `recomp_entrypoint` — has nothing to do with menus, and is currently
blocked entirely by them.

## RT64 keeps its build settings at directory scope

Five separate settings had to be hoisted into the top-level `CMakeLists.txt`
because RT64 defines them with directory scope, where a sibling directory such
as RecompFrontend cannot see them. Each produced a failure that pointed
somewhere other than the cause:

| Setting | Symptom when missing |
|---|---|
| `DXC` | Build rule tried to execute the `.hlsl` file as the compiler; empty output; failure reported later in `file_to_c` |
| `DXC_*_OPTS` | `dxc failed : Target profile argument is missing` |
| `SDL2_INCLUDE_DIRS`, `sdl2_SOURCE_DIR` | `'SDL.h' file not found`, in two different mechanisms (recompui uses one, recompinput the other) |
| `__PRFCHWINTRIN_H` | `definition of builtin function '_m_prefetch'` in SDL's headers — RT64 compiles, consumers do not |
| `RT64_STATIC` | Dozens of undefined `RT64::Application::*` at link: the default shared build's import library exports none of them |

The target profile one is worth a second look: RT64 writes `"-T vs_6_3"` as a
single string, which works inside its own directory but arrives at `dxc` as one
argument from elsewhere. The fix is to pass `"-T" "vs_6_0"` as two tokens — an
error message about shaders that is really about CMake quoting.

None of this was patched into the submodules; `lib/` stays a clean upstream
checkout and the compensation lives in this repository's own build file.

## Other integration contracts discovered

RecompFrontend expects things from a port by name, with no interface to
implement and no error if they are missing until link or run time:

- `patches/ui_funcs.h` — included by a hard-coded relative path
  (`../../../../../patches/ui_funcs.h`) and marked "TODO: Forced game includes"
  upstream. It must also pull in `recompui/event_structs.h`, because the event
  dispatcher uses those types without including them itself.
- A global `SDL_Window* window` — declared `extern` in `ui_state.cpp`.
- A global `std::vector<recomp::GameEntry> supported_games` — the launcher links
  against it to list the game and drive its own "Load ROM" flow. Internal
  linkage compiles fine and fails at link with one unexplained symbol.

## What the recompiler agreed with

Worth recording as independent confirmation of phase 01: N64Recomp's generated
section table describes exactly the segment map recovered there, arrived at from
the ELF rather than from the loader —

```
rom 0x001000 -> ram 0x80000400  size 0x01CCC0   boot
rom 0x01DCC0 -> ram 0x80025C50  size 0x0A7F30   main
rom 0x0C5BF0 -> ram 0x800F64A0  size 0x00AE30   aux
```

— with `num_relocs = 0` on every entry, and a single `-1` overlay index. That is
the recompiler agreeing that nothing here relocates and there are no overlays.

## Deliberate simplifications to revisit

- **Audio is silent by construction.** `src/rsp.cpp` reports audio tasks
  complete without synthesising anything, because Rayman 2's audio microcode has
  not been through RSPRecomp. Returning `nullptr` instead would terminate the
  program on the first audio task, which would block this gate over phase 05
  work. The SDL device and rate handling are wired up regardless, so the ucode
  is the only missing piece when phase 05 arrives.
- **Input is a fixed keyboard map.** Enough to drive boot and menus; remapping
  and gamepads come with the frontend's input tab.
- **`__osGetSR` returns 0.** Still the first suspect if early execution goes
  astray once the game thread actually starts — see `src/port_runtime.cpp`.
