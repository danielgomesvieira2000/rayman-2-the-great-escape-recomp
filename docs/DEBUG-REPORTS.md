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

    RAYMAN2_SELFTEST=crash          raises an access violation
    RAYMAN2_SELFTEST=terminate      throws a C++ exception through noexcept
    RAYMAN2_SELFTEST=nullcall       asks for the function at 0x00000000
    RAYMAN2_SELFTEST=nullcall-live  the same, from inside the running game

Each exercises one of the paths and should leave a report containing a crash
block. None of them can fire by accident.

The last two are the one the recompiled game itself can reach: an indirect call
through a pointer that resolves to no function. It is worth knowing what that
looks like, because until v0.2.0-alpha it did not look like itself -- it printed
one line into the mirror pipe and called `std::exit` from a game thread, and the
teardown that followed crashed *again* underneath the live renderer and reported
that instead. See docs/issues/003.

`nullcall` fires before the game has started and so proves the reporting alone.
`nullcall-live` fires from a game thread once the camera is building a
projection, which is the only way to see the part of the block that matters:

## The call history in a crash block

Every indirect call in the recompiled game -- every `jalr`, every jump table --
resolves its target through one function in the runtime. That function keeps the
last 32 addresses each thread resolved, and every crash block prints them:

    the last 32 addresses this thread called indirectly, oldest first.
      0x800838AC          0x80084930 x2       0x800838AC          0x80084930 x2
      0x800838AC          0x80084930 x2       0x800841A4          0x80084930 x4
      ...

These are **game** addresses, so they are worth more than the native stack above
them: they resolve against this repository rather than against a matching debug
build. `grep -rn "glabel func_800838AC" asm/` names one; the libultra ones are in
`recomp/symbol_addrs.txt`.

The history is per-thread, because a fault happens on one thread and is not
helped by three others shuffled into it -- and a fault on the renderer or the
event pump simply has none, in which case the section is left out. Runs of the
same address are collapsed with a count, so a callback invoked from a loop
cannot flush the window and leave 32 copies of itself.

## Finding an address to cheat at

A cheat is a repeated write into the game's own memory, so every cheat needs an
address, and this port has no symbols for the game's data.

`src/demo_scan.cpp` can find one unattended, but only when the two states it has
to tell apart are visible from outside -- it separated the title screen from a
demo using the display-list rate. Most values have no such signal: nothing the
port can see says "Rayman was damaged just now". A person playing knows, so for
those the labelling has to come from them.

`RAYMAN2_MEMSEARCH=1` is that: the standard cheat search, with the port doing
the scanning and the player supplying the labels.

    F5   start (or restart): remember every word in RDRAM
    F6   keep only what went DOWN since the last snapshot
    F7   keep only what is UNCHANGED since the last snapshot
    F8   keep only what went UP since the last snapshot

`RAYMAN2_MEMSEARCH=selftest` drives the same machinery on a timer instead of on
keys, to prove the tool works before anyone spends time using it.

For health: stand somewhere safe at full health and press **F5**. Take a hit,
press **F6**. Take another, **F6**. Walk around without being hit, **F7**.
Usually four or five presses is enough for the list to be short enough to print.

**Alternate F6 and F7; neither is strong on its own.** Measured with
`RAYMAN2_MEMSEARCH=selftest`, "unchanged" against the whole of RDRAM removes only
about a quarter of it -- most of memory is code, textures and unused space that
was never going to change. "It went down" is likewise true of a great many words
in a running game: timers, positions, counters.

What almost nothing satisfies is *both in sequence*: it went down exactly when
Rayman was damaged, then held still exactly while he was not. The big drops come
from the **F7 that follows an F6**, so alternate them rather than repeating
either.

### Or take the address from an existing cheat code

The search is the fallback. If a GameShark code for this game already exists,
the address in it drops straight in, because this port keeps RDRAM exactly as
the console does -- same addresses, same layout, same byte order.

An N64 GameShark code is `80XXXXXX 00YY`: *write the byte `YY` at RDRAM offset
`XXXXXX`*. The offset is the KSEG0 address with its top byte already there, so

    80123456 0005   ->   RAYMAN2_HEALTH_ADDR=0x80123456
                         RAYMAN2_HEALTH_WIDTH=1
                         RAYMAN2_HEALTH_VALUE=5

`81XXXXXX` is the two-byte form, so `RAYMAN2_HEALTH_WIDTH=2`. **The width is not
a detail:** most such codes are byte writes, and writing four bytes at a byte's
address holds the intended value and flattens the three beside it, which belong
to something else.

Codes are version-specific. This port targets **NUS-NY2E rev 0 (USA)**, so a
code for the USA release should match and one for a PAL release will not.

### Confirming a candidate

Launch with `RAYMAN2_HEALTH_ADDR=0x........` (and the width) and the port reports
what is at that address once a second, whether or not the cheat is on. Play, and
watch: an address that falls when Rayman is damaged and rises when he is healed
is health. One that does not is a coincidence that survived the search. When it
is right, it goes into `src/cheats.cpp`.

## Soaking an intermittent failure

Some failures are not reproducible one run at a time. The intro crash in
docs/issues/003 is the example: three launches on the reporter's machine gave
two crashes and one clean nine-minute session, on the same build with the same
inputs. Nothing about that can be worked on, or confirmed fixed, from a single
run.

    scripts/soak.sh --runs 20

launches the port, lets it run the intro, kills it and tabulates how each run
ended, keeping every report. It drives `build-headless` by default because that
build starts the game itself; to soak the **shipped** configuration -- which is
what every report so far has come from -- point it at the frontend build and let
it press Start:

    RAYMAN2_AUTOSTART=1 scripts/soak.sh --build build-fe --runs 20

Anything exported reaches the runs, which is how a hypothesis gets tested: soak
once as-is, once with `RAYMAN2_NOAUDIOUCODE=1`, once with `RAYMAN2_YIELD_MS=10`,
and compare the three numbers.

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

**Press F1.** It is on in every build, released or not, with nothing set
beforehand — see [DEBUG-MENU.md](DEBUG-MENU.md), which also covers the port's own
window that opens beside it.

It has to be on unconditionally rather than offered as a setting. RecompFrontend
registers the "Dev Mode" option as *hidden*, so there is no checkbox in the
Graphics tab to find; and it cannot be turned on while the game is running,
because RT64 installs the message hook that delivers these keys when the renderer
is built, from a value read once. Flipping it later does nothing at all and looks
exactly like the feature not existing. `RAYMAN2_DEVMODE=0` turns it off.

    F1   the frame inspector: pause, and walk framebuffer pairs -> projections
         -> draw calls. Highlighting a call shows which geometry it is; tiles,
         textures and samplers are inspectable. The port's own readout opens
         with it.
    F2   deliberately unbound -- upstream RT64 toggles ray tracing for the whole
         session here, with nothing on screen to say so
    F3   view RDRAM directly
    F4   pause and resume the frame

The Debugger tab also carries the frame's draw call and triangle counts, which
are worth watching on their own: a step in the count as the camera pans past the
moment something vanishes says the game stopped submitting it, and a steady
count says it is still being sent and lost later.

**The free camera is driven by the middle mouse button**, which is not written
down anywhere and looks exactly like a broken feature if you try to fly it with
the keyboard:

    middle-drag              pan
    Left Ctrl + middle-drag  move forwards and back
    Left Alt + middle-drag   rotate

It is ignored while the cursor is over the panel, so drag on the game itself.
Tick "Free Camera Enabled" first.

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

    RAYMAN2_SELFTEST=                  prove the crash reporting works (above)
      crash|terminate|nullcall
      |nullcall-live
    RAYMAN2_NO_MIRROR=1                do not mirror output into the report;
                                       errors and crashes are still recorded
    F9  (a key, not a variable)        capture a graphics issue: screenshot,
                                       VI framebuffer, settings and an issue
                                       stub, into debug-report/captures
    F1  (a key, not a variable)        the debug menu: RT64's Game editor and
                                       the port's own read-only readout of
                                       every facility below. On in every build;
                                       see docs/DEBUG-MENU.md
    RAYMAN2_DEBUGMENU=0                turn the port's half of F1 off; RT64's
                                       Game editor stays
    RAYMAN2_DEVMODE=0                  turn F1 off entirely. It is ON by
                                       default in both builds, because a debug
                                       menu that needs a relaunch to reach is
                                       one nobody has when they need it
    RAYMAN2_AUTOSTART=1                start the game without waiting for the
                                       launcher; what makes the shipped build
                                       scriptable (scripts/soak.sh)
    RAYMAN2_NOAUDIOUCODE=1             do not run the audio microcode: audio
                                       tasks complete, the game is silent
    RAYMAN2_YIELD_MS=<n>               how long a spinning game thread waits
                                       before yielding (default 1, max 100)
    RSPDMATRACE=1                      list every distinct 4 KB RDRAM page a
                                       microcode reads or writes
    RAYMAN2_FRAMECAP=<n>               hold the game's loop to n display lists a
                                       second. OFF by default: it fixes the
                                       attract-mode demos, which run at double
                                       speed, and costs the rest of the game
                                       half its frames, because there is no
                                       working way yet to apply it only to the
                                       demos. 30 turns it on -- docs/issues/004
    RAYMAN2_DEMOSCAN=1                 search RDRAM for the attract-mode flag by
                                       labelling samples from the display-list
                                       rate. How the flag above was found
    RAYMAN2_MEMSEARCH=1                find a value in the game's memory by
                                       playing: F5 to start, then F6 (it went
                                       down), F7 (unchanged) or F8 (it went up)
                                       after each change. How a cheat's address
                                       is found
    RAYMAN2_HEALTH_ADDR=0x........     tell the Cheats tab where Rayman's health
                                       is, without a rebuild, to confirm a
                                       candidate from the search
    RAYMAN2_HEALTH_VALUE=<n>           the value to hold health at (default: the
                                       highest seen so far)
    RAYMAN2_HEALTH_WIDTH=1|2|4         how many bytes the cheat writes
                                       (default 1, which is what a GameShark
                                       80XXXXXX code writes)
    RAYMAN2_WATCH=0xADDR[,0xADDR..]    print those words once a second with the
                                       display-list rate beside them
    RAYMAN2_PACEMARGIN=<ms>            how far before the field boundary a
                                       capped frame completes (default 6; only
                                       meaningful with the cap on)
    RAYMAN2_PACEPROBE=1                report the gap between consecutive game
                                       frames in VI fields -- one bucket means
                                       the pacing is even, several means judder
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
