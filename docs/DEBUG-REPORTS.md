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

## Environment variables

    RAYMAN2_SELFTEST=crash|terminate   prove the crash reporting works (above)
    RAYMAN2_NO_MIRROR=1                do not mirror output into the report;
                                       errors and crashes are still recorded
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
