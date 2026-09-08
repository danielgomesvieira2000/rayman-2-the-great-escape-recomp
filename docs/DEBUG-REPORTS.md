# Debug reports

Every time the port runs, it writes one text file describing that session. The
files live in a `debug-report` folder next to the executable, named for when the
session started plus a short session id:

    debug-report/2026-09-08_073353-7c28af16.txt

If the folder beside the executable is not writable -- an install under Program
Files, typically -- the reports go to `debug-report` inside the per-user
configuration folder instead, and the port prints the path it chose on its first
line. The newest fifty are kept and older ones are deleted, so the folder never
needs tidying.

## What to do with one

Send the whole file. That is the entire procedure.

The file is written to be read equally well by a person and by an AI assistant,
which is why it is arranged the way it is: the build and the machine come first,
because those are the questions that otherwise have to be asked before anything
else can start; then every event in order with a timestamp measured from launch;
then a summary saying whether the session ended cleanly.

Quoting a fragment is usually worse than sending the file. The lines *before* a
failure are frequently more informative than the failure itself, and they are
the first thing to be lost when someone copies out "the error".

## What is in it

  * **SESSION, BUILD, SYSTEM, GAME** -- the session id and start time, the port
    version and whether it is the frontend or headless build, the operating
    system, CPU, core count and memory, the configuration folder, and whether a
    ROM was already ingested.

  * **EVENTS** -- every line the port and its libraries printed, mirrored into
    the file as it happened and timestamped. This is what makes a report carry
    RT64's and librecomp's own messages and not only the ones the port thought
    to record.

    Each line carries a level. `LOG` is ordinary output. `INFO` is something the
    port recorded deliberately. `ERROR` is either an error the port raised
    itself -- everything the player was shown in a message box is one -- or a
    mirrored line that names a failure. `WARN` likewise. `CRASH` is a block.

  * **SUMMARY** -- when it ended, how long it ran, how many errors, warnings and
    crashes, and whether it finished cleanly.

    A file with **no SUMMARY** was killed or died hard enough that it never got
    to write one. That is itself a finding, and worth mentioning when sending it.

## Crashes

A crash writes a block bracketed by `BEGIN CRASH REPORT` and `END CRASH REPORT`,
containing the exception and its code, the faulting address resolved to module
plus offset, the thread, what the operating system believes is mapped at the
address that faulted, and a stack walk.

Two paths produce one. Ordinary faults -- access violations and the rest -- come
through the unhandled-exception filter. Uncaught C++ exceptions, including one
thrown out of a `noexcept` function, arrive with code `0xE06D7363` and are
labelled `CPP_EXCEPTION`; that one is worth naming because it terminates the
process without running most handlers, and it is the shape of failure that cost
this project the most time before it was recognised.

Crash blocks are written straight to the file rather than through the output
mirror, because a process that is crashing cannot be relied on to keep pumping
its own output.

Addresses read as `module+offset`. To turn one into a function name, against the
exact build the report names:

    llvm-symbolizer --obj=rayman2-recomp.exe --relative-address --demangle <offset>

That needs a build with debug information (`-DCMAKE_BUILD_TYPE=RelWithDebInfo`);
a plain release build resolves only to the nearest exported symbol.

## Checking that it works

Crash reporting is the one feature that cannot be verified by using the program
normally: it runs only when something has already gone wrong, and a reporter
that is silently broken produces exactly the same file as a session with no
crashes in it. So it can be asked to prove itself:

    RAYMAN2_SELFTEST=crash      raises an access violation
    RAYMAN2_SELFTEST=terminate  throws a C++ exception through noexcept

Each exercises one of the two paths and should leave a report containing a crash
block. Neither can fire by accident.

## Graphics issues

A crash reports itself. A *graphics* bug does not: a screenshot says that
something is wrong, never why. In a recompilation the why is nearly always one
specific thing — a GBI command RT64's F3DEX 1.x path mishandles, a combiner or
render mode, a texture tile setup, a framebuffer or VI interaction — and the
distance between "I can see it" and "I can fix it" is knowing *which draw call*.
Everything here is about closing that distance cheaply.

### Press F9 while it is on screen

One keypress writes, into `debug-report/captures/<time>/`:

  * `screen.bmp` — what you were looking at;
  * `vi-framebuffer.bin` — what the game actually drew, straight out of RDRAM,
    before any scaling or widening. The difference between the two is itself
    diagnostic: a defect present in both came from the game or the display list,
    one present only on screen came from the renderer. Convert it with
    `tools/fb_to_png.py`, using the dimensions the stub records;
  * `ISSUE.md` — a filled-in issue stub carrying every graphics setting that was
    in force, so "does it change with resolution, aspect or antialiasing" never
    has to be asked.

Evidence taken at the moment is worth far more than evidence reconstructed
afterwards, when the spot is gone and the settings have been fiddled with.

### The three-toggle triage

Thirty seconds, one setting at a time in the Graphics tab, and it eliminates
most of the search space before anyone reads code:

  * **Internal resolution** — if the defect scales with it, it is a renderer or
    upscaling issue rather than a display-list one.
  * **Aspect ratio**, Expand against Original — if it only appears widened, it
    is culling or 2D anchoring.
  * **Antialiasing** off — if it vanishes, it is a coverage or edge issue.

### RT64's frame inspector

The port ships a full frame debugger and it is switched off by default. It is
also not where you would look for it: RecompFrontend registers the "Dev Mode"
option as *hidden*, so there is no checkbox in the Graphics tab, and it cannot
be turned on while the game is running — RT64 installs the message hook that
delivers these keys when the renderer is built, from a value read once, so
flipping it later does nothing at all and looks exactly like the feature not
existing.

Launch with it on instead:

    RAYMAN2_DEVMODE=1

(or set `"developer_mode": true` in `graphics.json` to have it on permanently).
The port prints a line confirming it. Then:

    F1   the frame inspector: pause, and walk framebuffer pairs -> projections
         -> draw calls. Highlighting a call shows which geometry it is; tiles,
         textures and samplers are inspectable, and there is a free camera.
    F3   view RDRAM directly
    F4   texture replacements

You do not have to understand what you are looking at. Pausing on the bad frame
and screenshotting that panel is already far more useful than a screenshot of
the game, and one line — "draw call 37 in projection 1 is the water" — is
usually enough to act on immediately.

### Filing it

Copy `docs/issues/TEMPLATE.md` to `docs/issues/NNN-short-name.md`, drop the
capture folder's contents beside it, and commit. In the repository rather than
in a message, because it survives the session, the notes and the images can be
read together, and once it is fixed the file is the regression record.

The single most valuable thing to include is a way to reproduce it: a Controller
Pak image parked just before the spot, so reaching it costs seconds instead of
half an hour.

## Environment variables

    RAYMAN2_SELFTEST=crash|terminate   prove the crash reporting works (above)
    RAYMAN2_NO_MIRROR=1                do not mirror output into the report;
                                       errors and crashes are still recorded
    F9  (a key, not a variable)        capture a graphics issue: screenshot,
                                       VI framebuffer, settings and an issue
                                       stub, into debug-report/captures
    RAYMAN2_DEVMODE=1                  RT64's developer UI and frame inspector
                                       on F1 (there is no checkbox for this)
    RAYMAN2_DDPROBE=1                  report the game's projection arguments
    RAYMAN2_DRAWDIST=<n>               scale the projection far plane; see
                                       docs/issues/002, it changes nothing
                                       visible in this game
    RAYMAN2_PRESENT=skipbuffering      presentation mode; unlocks RT64 frame
                    |presentearly      interpolation, but tears on this game.
                                       Default (unset) is console.
    RAYMAN2_FPSPROBE=1                 report presented frames per second and
                                       the rate RT64 measured from the display
    RAYMAN2_PAKTRACE=1                 trace every Controller Pak transaction
    RAYMAN2_AUDIOPROBE=1               report the audio rate, peak and queue
                                       depth once a second

## Privacy

A report contains the port's own output, your Windows version, your CPU, core
count and memory, and the file paths the port uses -- which include your user
name if the port is installed under your home directory. It contains nothing
else about you and nothing from anywhere else on the machine.
