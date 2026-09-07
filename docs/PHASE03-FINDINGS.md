# Phase 03 findings — runtime harness

> **Update: the port now brings RT64 up on its own, without the frontend.**
> `src/rt64_context.cpp` implements ultramodern's `RendererContext` directly on
> `RT64::Application`, and a build configured with `-DRAYMAN2_ENABLE_FRONTEND=OFF`
> reaches `[rayman2] RT64 ready (graphics api 1)` every time. That removes the
> frontend from the critical path, which is what this phase was blocked on.
>
> **The gate is still not met, and one run must not be mistaken for meeting it.**
> A single run did reach the game — see *One run reached the entrypoint* below —
> but the current build does not, and I was unable to get back to it. Read that
> section before trusting anything here.

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


---

# Update — writing our own RT64 context

## What it changed

`src/rt64_context.cpp` is the port's own renderer context: RCP register block,
`RT64::Application` construction, `setup()`, and the nine `RendererContext`
methods. It is deliberately minimal, and in particular does **not** copy the
workarounds a mature port accumulates — deferring MSAA to the next launch,
routing pause state into RT64, choosing a presentation mode. Those are findings
about other games; copying them here would be untestable until this game renders
and indistinguishable from deliberate choices afterwards.

With `-DRAYMAN2_ENABLE_FRONTEND=OFF` the port now gets past renderer bring-up
reliably:

```
[rayman2] start
[rayman2] entering recomp::start
Device Name: Intel(R) Iris(R) Xe Graphics
[rayman2] RT64 ready (graphics api 1)
```

This confirms the diagnosis above: the frontend was the blocker, not the game
path. A sixth directory-scoped RT64 setting turned up on the way —
`HLSL_CPU`, without which RT64's dual HLSL/C++ headers give a wall of
`unknown type name 'uint'`.

## One run reached the entrypoint

One run, with the audio-subsystem fix in and launched from a console, produced:

```
[rayman2] RT64 ready (graphics api 1)
Initializing recomp heap at offset 0x01000000 with size 0x1F000000
[rayman2] entering recomp_entrypoint -- the game thread is running
Failed to find function at 0x80000450
```

That is the gate's condition, and the failure after it was a good one:
`0x80000450` is where the entry stub's `jr $t2` goes. splat sizes
`func_80000400` at `0xC0`, swallowing the function that starts there; because
the jump is indirect, splat never saw a call to it and started no function, so
librecomp cannot resolve it at run time.

**But that state is gone and I could not restore it.** Declaring the boundary
(`func_80000400 size:0x50` plus `func_80000450`) made things worse — an access
violation *before* the heap is initialised, earlier than the problem being
fixed. Reverting the declaration did not bring the good state back. Restoring
`recomp/symbol_addrs.txt` byte-for-byte to its contents at the time of that run
did not either. The current build fails identically on 4 of 4 and 3 of 3 runs:
exit `0xC0000005`, never reaching `recomp_entrypoint`.

So the honest position is: the gate was reached once, it is not reached now, and
the difference is not explained by any source change I can identify. Claiming
the gate on the strength of that single run would be wrong.

## Correction: the build WAS reproducible; I was wrong

The section that stood here claimed the generated C could not be reproduced from
the committed tree, and blamed a feedback loop through `auto_funcs.txt` and
`ignored_syms.txt`. **That was wrong, and it was asserted without being
measured.** `tools/manifest.py` now hashes every pipeline input and output, and
the measurement is unambiguous:

```
REPRODUCIBLE: every input and output matches the recorded manifest.
```

Splitting, assembling and recompiling twice from the same ROM and the same
committed configuration produces byte-identical `asm/`, ELF and
`RecompiledFuncs/`. The supposed feedback loop is not even operating:
`auto_funcs.txt` contains no declarations at all, because the JAL-target pass
found every call target already classified.

The manifest is kept regardless. It is what turned a guess into a fact in a
single command, and phase 04 needs that guarantee to be checkable rather than
assumed. `scripts/recompile.sh` writes it on every run; `python
tools/manifest.py check` compares a later run and exits non-zero if outputs move
while inputs do not.

## What was actually happening

Running the same binary ten times:

```
reached recomp_entrypoint: 1 / 10
  0xC0000005 x9      access violation
  0xC0000409 x1      fail-fast, after "Failed to find function at 0x80000450"
```

So the *program* is non-deterministic, not the build. The one run that reached
the game was not progress that later regressed -- it was the one run in ten that
got the game thread started before the renderer thread crashed.

The renderer thread access-violates either way. That is the bug.

An attempt to fix it as a startup race -- waiting for the renderer's first
presented frame instead of `main()`'s blind 500 ms sleep before `start_game` --
made the failure **deterministic in the other direction**: 0 of 10 now reach the
entrypoint, all with `0xC0000005`. That is the expected consequence if the
renderer dies before ever presenting a frame, since the wait then never
completes. It is kept anyway, for two reasons: waiting on a real signal is
correct where sleeping a guessed interval is not, and a failure that reproduces
every time is worth more than one that hides nine times in ten.

## Correction: the crash is not in the renderer, and not a null check

The previous section predicted the fault was in `src/rt64_context.cpp` --
`update_screen`, `send_dl` or `send_dummy_workload` dereferencing `app` with no
guard, on the renderer thread. **That was wrong.** Those guards were a genuine
defect and are now fixed, but adding them changed nothing: still 0 of 10.

`src/crash_report.cpp` was added to settle it. There is no debugger installed on
this machine, and `0xC0000005` with no output is close to no information, so the
port now installs an unhandled-exception filter that prints the faulting
address, whether it was a read or a write, and a stack walk resolved to module
and offset. Built once with `-DCMAKE_BUILD_TYPE=RelWithDebInfo`, those offsets
resolve through the PDB with `llvm-symbolizer`:

```
recomp::do_rom_read()        librecomp/src/pi.cpp:72
init()                       librecomp/src/recomp.cpp:505
wait_for_game_started()      librecomp/src/recomp.cpp:716
recomp::start lambda         librecomp/src/recomp.cpp:980
```

That is the **game thread**, not the renderer: librecomp emulating IPL3 by
DMAing the first 1 MB of ROM into RDRAM, before the entrypoint is ever called.
Which is also why `entering recomp_entrypoint` never printed -- the game never
got that far.

## A real bug found on the way: the ROM was never loaded

The diagnostic that made this obvious:

```
pre-start: rom_valid=1 rom_loaded=0 rom_bytes=0
```

`check_all_stored_roms()` and `is_rom_valid()` establish only that a correct
dump exists in the config directory. **They do not read it, and neither does
`start_game()`** -- the port is expected to call `recomp::load_stored_rom()`,
and `main()` never did.

That is not a quiet no-op. `do_rom_read` computes its source as
`rom.data() + physical_addr - rom_base`; with an empty span that is an offset
from `nullptr`, and the 1 MB copy walks into unmapped memory. Fixed, and the
port now reports `rom loaded: 33554432 bytes` before starting.

## What is still wrong

The ROM fix was necessary and not sufficient. With 32 MB correctly loaded, the
same write still faults at the same place, and the numbers say it should not:

- The faulting write is at `rdram + 0x403`. That is right: `MEM_B` maps KSEG0
  `0x80000400` to offset `0x400`, byte-swapped to `0x403`. It is the fourth byte
  of the copy, not a wild pointer.
- librecomp allocates RDRAM as a 4 GB reservation and makes the first
  `mem_size` -- **512 MB** (`librecomp/include/librecomp/addresses.hpp`) --
  `PAGE_READWRITE`. Offset `0x403` is far inside that.
- The allocation did not fail. Failure prints "Failed to allocate memory!"
  through the port's own message box, and it never appears.

So the ROM is loaded, the destination offset is correct, and the destination
region is nominally writable -- and the write still faults. One of those three
statements is false at run time, and finding out which is the next step. It
wants a debugger stepping into `pi.cpp:72` to read the actual `rdram` value and
query it with `VirtualQuery`, rather than more reasoning from the outside. The
4 GB `MEM_COMMIT` reservation on a laptop is the first thing worth checking.

## What phase 04 inherits

- **A crash reporter that works.** Every fault from here on names a function and
  a line instead of an exit code. This is the single most useful thing added in
  this phase.
- **A `RelWithDebInfo` build** (`build-dbg`) that produces a PDB, and
  `llvm-symbolizer` from the LLVM install to resolve it.
- **A deterministic failure.** It reproduces on every run, which it did not
  before.
- `Failed to find function at 0x80000450` is still real and still unfixed, but
  it is downstream of this and only observable once the DMA succeeds. Declaring
  that boundary was tried and made things worse -- see `recomp/symbol_addrs.txt`.
