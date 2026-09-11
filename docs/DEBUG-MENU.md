# The debug menu

**Press F1.** In any build, released or not, with nothing set beforehand. F1
again closes it.

Two windows appear together, and that pairing is the point:

| Window | Answers |
|---|---|
| **Game editor** (RT64's) | What the *renderer* did. Pause the game, right-click a pixel to list the draw calls under it, inspect framebuffers and textures, turn widescreen or upscaling off to isolate a fault. |
| **Rayman 2 Debug** (the port's) | What the *port* is doing. Every facility this port has, and its state right now. |

RT64's half has always been there. The port's half is new, and this file is
about that.

---

## Why it exists

This port has a lot of instrumentation and, until now, no way to look at any of
it while the game was running. Every facility reports itself the same way: a line
printed to the log when something happens. That is right for an event and wrong
for a state. "Is the frame cap on?", "is the camera being widened for culling
right now?", "did that capture actually write anything?" are questions about
*now*, and answering them from a scrolling log means reading backwards for the
last line that mentioned it and hoping nothing has changed since.

Worse, most of those facilities only report at all when an environment variable
was set before launch. So the question that comes up while playing gets answered
by quitting, setting a variable, relaunching, and getting back to the same place
— by which time the thing that prompted it is gone.

---

## It is read-only

Nothing in this window changes what the port is doing. That is a deliberate first
step, not an unfinished one.

A debug menu that can also set things is a better tool and a worse *instrument*:
the first question about any odd behaviour becomes "did I touch something in the
menu?", and the menu joins the list of suspects. The readouts come first, and
they are what will say which controls are worth having. Anything that needs
changing today is still changed the way it always was — an environment variable,
listed in [DEBUG-REPORTS.md](DEBUG-REPORTS.md).

---

## What it shows

```
Rayman 2 Debug
Read-only. F1 closes this and RT64's Game editor together.
▼ Session
  build                 frontend
  uptime                0:00:26
  errors / crashes      0 / 0
  config                C:\Users\...\AppData\Roaming\rayman2-recomp
  session report        ...\debug-report\2026-09-11_204602-594a4a71.txt
▼ Renderer
  display lists         739  (59.8/s)
  frames presented      1589  (59.8/s)
  this menu             60.1 fps
  frame cap             off
▼ Graphics settings
  aspect ratio          Expand
  HUD ratio             Full
  ...
▼ Draw distance & widescreen
▼ Capture
▼ Controller Pak
▶ Memory search
▶ Attract-mode scan
```

| Section | Worth knowing |
|---|---|
| **Session** | `build` is `frontend` or `headless`. The two differ in who owns the RT64 application and whether there is a launcher, and a screenshot of this window is ambiguous without it. The two paths are the ones to quote when reporting anything. |
| **Renderer** | **display lists** is the *game's* frame rate — one per frame it draws. **frames presented** is the *renderer's*. They match under the default presentation and diverge the moment RT64 interpolates or the game stalls while the window keeps painting; that difference is the fastest way to tell "the game has stopped" from "the window has stopped". |
| **Graphics settings** | The live `GraphicsConfig`, named as the frontend names it. `downsample option` is reported as the raw value rather than as a multiplier, because it has been seen disagreeing with RT64's own panel. |
| **Draw distance & widescreen** | See below — the three aspects are easy to misread. |
| **Capture** | How many F9 captures this session, and where they went. |
| **Controller Pak** | Whether port 1 presents one. It always does, and that is why this port has no rumble — see [CONTROLLER-PAK-FINDINGS.md](CONTROLLER-PAK-FINDINGS.md). |
| **Memory search** | Collapsed, and empty unless `RAYMAN2_MEMSEARCH=1`. Shows whether the search has started, how many candidates survive and how many narrowings you have done. |
| **Attract-mode scan** | Collapsed, and empty unless `RAYMAN2_DEMOSCAN=1`. |

### Reading the widescreen numbers

Four fields, and the first three all read "about 1.33" until the window is
widened, which makes them easy to run together. They are:

| Field | Is |
|---|---|
| `aspect: first asked` | The very first aspect the game passed `guPerspective`. Kept forever as the reference. |
| `aspect: last asked` | The most recent one. |
| `aspect: window` | The window's own, as the port measured it. |
| `fov widening` | **The one that says whether the fix is doing anything.** `1.00x` means the camera is being left alone. |

None of them is "the aspect the port supplies", because the port supplies none:
writing the aspect back was tried four times and abandoned, since it never
reaches the game's visibility test. The fix widens the camera's *field of view*
so the game culls against what a widescreen player can see, then undoes the
widening in the finished matrix so the framing is unchanged. `fov widening` is
that factor. See `src/draw_distance.cpp` and [issues/001](issues/001-widescreen-edge-culling.md).

The first version of this readout labelled one of these "given to cull" and was
simply wrong. If a field here ever seems to say something surprising, check it
against the file that fills it before believing it.

---

## The other keys

Developer mode is on in every build, which arms RT64's other shortcuts for
everyone. They are therefore part of the shipped build and were each decided on
its own merits:

| Key | Does |
|---|---|
| **F1** | Opens and closes both windows |
| F2 | **Unbound.** RT64 uses it to toggle ray tracing for the whole session, with no menu entry saying so and nothing on screen to explain what changed. `tools/patch_rt64_debug_menu.py` removes the case from both key filters, so the key passes through to the game like any other. |
| F3 | Views RDRAM. Visibly reversible, plainly diagnostic. |
| F4 | Toggles texture replacements. Same. |
| F5–F8 | The memory search, when it is armed. |
| F9 | Capture a graphics issue. |

`RAYMAN2_DEBUGMENU=0` turns the port's window off; RT64's stays on F1.
`RAYMAN2_DEVMODE=0` turns off both.

---

## How it is built

Five pieces. Only the first is RT64-specific, and the shape is worth knowing
because the same approach suits the sibling ports.

### 1. A hook in RT64, because RT64 owns the ImGui context

A port cannot call `ImGui::Begin` of its own: RT64 creates the context, begins
the frame and ends it, and a call outside that window either asserts or draws
nothing. So `tools/patch_rt64_debug_menu.py` adds one function pointer that RT64
calls from inside its own frame, in `State::inspect()`:

```cpp
extern "C" void (*RT64_PortDebugMenuHook)() = nullptr;
```

Null unless the port sets it, so upstream behaviour is unchanged, and it costs
one predictable branch per frame in developer mode only.

**It is a script, not an edit.** `lib/RT64` is a submodule pinned to a fork
branch, and a hand edit is reverted without warning by the next
`git submodule update`. The script is idempotent and exits with a clear message
if the anchor text has moved upstream. **Re-run it after any submodule update**,
or the build fails at link time:

```
python tools/patch_rt64_debug_menu.py
```

### 2. Developer mode on, and left on

RT64 gates four separate things on `userConfig.developerMode`, and every one is
on the path between the F1 key and the window: whether it installs its Win32
subclass and SDL event filter at all, whether those filters look at F1, whether
the keystroke creates the inspector, and whether `State::inspect()` draws
anything. The flag is read once, at construction, so there is no halfway
position and no turning it on later.

Both `src/render_context.cpp` and `src/rt64_context.cpp` therefore force it on,
before `setup()`. A debug menu that only exists in a build made for it is a debug
menu nobody has when they need it: the person who has just seen something odd is
running the game they downloaded.

### 3. One struct per facility

`include/debug_status.h` declares a small plain struct for each facility, and the
file that *owns* the state fills it. That keeps every value one step from the
code that decides it, and it is why the menu has no logic of its own.

### 4. The threading contract

The facilities run on the thread that pumps events or on the game's; the menu
draws on the renderer's UI thread. So an accessor never reads live state across
that boundary — the owning thread publishes into an atomic and the accessor
copies it out.

`src/memory_search.cpp` and `src/demo_scan.cpp` both hold a `std::vector` of
candidates that the search rewrites as it narrows. Reading `.size()` on that from
the UI thread is a data race, so each publishes the count into a
`std::atomic<size_t>` instead, from the one place its state changes. Two relaxed
stores per keypress, and no race.

### 5. The window

Ordinary ImGui, drawn from the hook, in `src/debug_menu.cpp`. Sections are
`CollapsingHeader`s, open by default except the two that are empty unless armed.

---

## Keeping this current

This file describes a tool that lives in the port; if the tool changes, change it
here in the same commit. The environment variables it reports on are listed in
[DEBUG-REPORTS.md](DEBUG-REPORTS.md), and the reasons behind each facility belong
with that facility's own source file.
