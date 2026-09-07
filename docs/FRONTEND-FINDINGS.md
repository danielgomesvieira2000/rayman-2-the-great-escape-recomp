# The frontend, brought forward from phase 06

RecompFrontend is listed under phase 06, but it was integrated early, at the
phase 05 gate, because the gate is "the first three levels play start to finish"
and playing them by hand needs a launcher and rebindable controls. Nothing else
about phase 06 was pulled forward.

The integration itself is small -- `src/frontend.cpp`, an adapter in
`src/render_context.cpp`, and asset staging in `CMakeLists.txt`. What follows is
the part that was not small: three failures that each present as the frontend
quietly doing nothing, and which no amount of reading the port's own code would
have explained.

## The order of construction is load-bearing

`recompui::config::init_modal()` throws when no configuration has been loaded:

    Configurations have not been loaded. Call recompui::config::finalize() first.

recompui builds its menus from inside RT64's setup, through a render hook it
installs before creating the application, so that throw happens on a renderer
thread behind a `noexcept` boundary. It produces no message, no crash dialogue
and no non-zero exit. The window is created and torn down, the setup call never
returns, and from the outside the port hangs with no window.

So: create every tab, then `finalize()`, and do both before `recomp::start`.
`profiles::initialize_input_bindings()` has to come before `finalize()` too --
nothing else calls it, and without it `finalize()` loads a saved `controls.json`
into no profiles at all, which showed up as a Controls tab listing every N64
input with every slot blank.

## "Load ROM" did nothing, because COM was not initialised

Clicking Load ROM highlighted the option and then did absolutely nothing: no
dialog, no error, no log line. The chain is recompui's `open_file_dialog` ->
`NFD_OpenDialogN` -> `CoCreateInstance(CLSID_FileOpenDialog)`, and COM is
per-thread. Nothing in RecompFrontend, librecomp, ultramodern or RT64 calls
`NFD_Init` or `CoInitializeEx` -- the frontend leaves that to the port. Without
it `CoCreateInstance` returns `CO_E_NOTINITIALIZED`, NFD returns `NFD_ERROR`,
and recompui's callback treats that identically to the player pressing Cancel.

The expensive half was the word *thread*. Three are involved, and they are all
plausible:

    main thread                 runs main() and recomp::start
    gfx thread                  runs ultramodern's gfx_thread_func, and calls
                                the port's create_render_context
    RT64 present-queue thread   runs the render hooks -- and so the menus

recompui draws its menus from RT64's draw hook and *also dequeues and dispatches
input events there*, so the click callback, and the dialog, run on the
present-queue thread. Initialising COM in `main()` and then in the renderer
factory both changed nothing; each attempt cost a build and looked identical
from outside. What settled it was a temporary probe printing
`std::this_thread::get_id()` at both ends together with NFD's own
`NFD_GetError()`, which reported "Could not create dialog" against three
distinct thread ids.

The fix is in `src/render_context.cpp`: read RT64's installed draw hook back
with `GetRenderHookDraw()`, chain it behind one of the port's own, and do the
`CoInitializeEx` in a `thread_local` initialiser. The `thread_local` is the
point -- it runs once on whichever thread RT64 chooses, so the port never has to
name that thread. The wrapper guards against wrapping itself, because recursing
in a hook called every frame is not a subtle failure.

## Fonts and icons are silent when missing

recompui loads `assets/promptfont/promptfont.ttf` by name during renderer
bring-up and draws every binding button's glyph in it. It is not part of RmlUi's
samples, so nothing staged it, and the Controls tab rendered a full grid of
empty squares -- the layout was right, the glyphs were simply not there. It is
committed under `assets/promptfont/` with its SIL Open Font License 1.1 text.
The menu icons under `assets/icons/` behave the same way: a missing file is a
missing button rather than an error.

## The screenshots were lying

`tools/grab_window.ps1` was DPI-unaware while the display runs at 125%. It read
the client rect in virtual coordinates and grabbed that many pixels off a screen
it also saw scaled; the two scalings do not cancel, and the result was the
top-left ~80% of the window saved at the right size. That looked exactly like a
UI laid out too large for its window, and sent a perfectly good launcher off to
be debugged. The tool now asks for per-monitor v2 awareness before it touches a
window. The same 125% is why an 800x600 window is 1000x750 physical pixels here.

## Where settings live

`main.cpp` calls `recomp::register_config_path()` with
`%APPDATA%\rayman2-recomp` before anything reads configuration. Without it
librecomp falls back to the working directory, and both librecomp and
RecompFrontend resolve every settings file relative to that one path -- the
ingested ROM, `controls.json`, and the per-tab config JSON.

Bindings are saved by the Controls tab's close handler and again by
`frontend_shutdown()`, and the defaults are written once if no file exists yet,
so a player who rebinds and then dismisses the whole menu with Escape does not
lose the change. Verified in both directions: a value edited on disk comes back
in the Controls tab, and a value changed in the tab is on disk afterwards.
