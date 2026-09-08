# 003 — the intro crashes: the game calls through a null function pointer

**Status:** open. The crash reporting around it is fixed; the null call itself
is not.

Reported by a player against v0.2.0-alpha, who sent three session reports from
one afternoon. The reports themselves are not kept in the tree -- they carry the
reporter's own paths -- so everything from them that the investigation rests on
is quoted below: the session ids, the timings, and the crash block.

---

## What is wrong

The game dies during the opening cinematic. Not every time.

## The three reports, side by side

Same build (0.2.0-alpha, linked 2026-09-08 12:23:57), same machine, same GPU
and driver, three launches inside nine minutes:

| report | ROM | pad | outcome |
| --- | --- | --- | --- |
| `2d6566a3` 12:31:41 | first run, none stored yet | none connected | died 257 ms after the game thread started |
| `9424b5b3` 12:35:32 | stored | connected | log stops dead at 6.2 s, **no crash block, no SUMMARY** |
| `cf7036e1` 12:40:30 | stored | connected | 9m29s, exited cleanly |

The two failures differ from the clean run in nothing the log records. The
missing pad and the Controller Pak creation in the first report are artefacts of
it being the first launch on that machine; the second report had a pad and an
existing pak and died anyway.

Same build, same machine, same inputs, three outcomes. **This is
non-determinism, not configuration**, and no amount of asking the reporter what
they did differently will find it.

## The fault

    [00:00:21.376] ERROR out: Failed to find function at 0x00000000

That line is `get_function()` in
`lib/N64ModernRuntime/librecomp/src/overlays.cpp`. The recompiled game executed
an indirect call -- `jalr`, or a `jr` through a jump table -- with 0 in the
register. There is no function at address 0 and there never will be, so the
lookup fails.

That is the whole of the real fault, and everything else in the report is a
consequence of how it was handled.

## Why the crash block in the report is misleading

`get_function` did this:

```c
fprintf(stderr, "Failed to find function at 0x%08X\n", addr);
assert(false);              // NDEBUG in a release build: compiles to nothing
std::exit(EXIT_FAILURE);    // on the game thread
```

`std::exit` runs the atexit table and every static destructor. It was called
from a game thread while the renderer, the SDL event pump, RT64, the audio
device and this port's own stderr-mirror pump were all still running, so the
teardown ran underneath live threads. The stack in the report reads exactly that
way: frame 20 is port code entering ucrtbase, frames 19 to 14 are ucrtbase's
onexit machinery, frames 13 to 7 come back into port code, and frame 7 faults
reading `[null+0x2c]`.

**So the `ACCESS_VIOLATION` the player sent is the shutdown, not the bug.** The
frames underneath it -- 31 to 21 -- are the recompiled call chain that made the
null call, and they are the only interesting part of the whole block. In a
release build they resolve to nothing.

`src/debug_report.cpp` carried a live instance of the same hazard: `g_pump` was
a namespace-scope `std::thread`, and `~thread()` on a joinable thread calls
`std::terminate()`. Nothing joined it on the `exit()` path.

## Why the second report is empty

`9424b5b3` has no crash block and no SUMMARY, and its last line is the ordinary
one every session prints. It did not stop there; it stopped being able to write.

Everything the process prints goes through a pipe drained by that same `g_pump`
thread (`start_mirror` in `src/debug_report.cpp`). A line printed moments before
a hard death is still sitting in the pipe when the process goes, and is lost.
That accounts for the missing "Failed to find function" line. The missing crash
block accounts for the rest: neither the unhandled-exception filter nor the
terminate handler ran to completion, which is what happens once teardown has
already begun consuming the machinery that writes them.

Read together, the two failures are one fault with two different teardown
orderings. The one that happened to survive long enough to write a block wrote a
block about the teardown.

## Is it stable?

Two failures in three launches, so roughly two in three -- but that is three
samples from one machine and should not be quoted as a rate. It needs a soak
run before any fix is believed.

## Fix, part 1: stop the second crash and make the first one legible

Done. Independent of the root cause, and worth having regardless.

1. `get_function` now calls a port-installed handler instead of exiting. The
   seam is `recomp::overlays::set_lookup_failure_handler` in the runtime fork.
2. The handler writes a real crash block -- what happened in words, the address,
   the thread, and a native stack walk -- then closes the report and tells the
   player where it is with a message box, instead of the window vanishing.
3. It leaves with `TerminateProcess`, never `std::exit`. The report is flushed
   and closed by then, so skipping the atexit chain costs nothing and removes
   the teardown race, the misleading second crash and the silent death in one
   move.
4. The `g_pump` destructor hazard is gone -- it is a raw pointer now, so the
   global has a trivial destructor -- and `end_session` is guarded against
   re-entry and against being called from the pump thread itself.
5. The mirror takes the crash mutex before writing a line, so a line the game
   printed a moment before the fault can no longer be drained into the middle of
   the crash block. That was visible the first time the new path was exercised:
   the self-test's own line landed between `kind:` and the first stack frame.
6. `RAYMAN2_SELFTEST=nullcall` exercises the whole path on demand, because crash
   handling is the one feature that cannot be checked by playing the game.

Verified: all three self-tests write a complete crash block and a SUMMARY, the
null-call path leaves with exit code 3 and no second crash, and the game still
runs the intro for fifty seconds at a steady 60 display lists a second.

After this, both reports above would have produced one file saying plainly that
the game called through a null function pointer, and where.

## Fix, part 2: find the null call

The instrumentation is done; the null call itself is still unidentified,
because it has not yet been reproduced on a machine here.

**A ring of resolved addresses**, done. Every indirect call in the recompiled
game -- every `jalr`, every jump table -- resolves through `get_function`, and
that function now keeps the last 32 addresses each thread resolved. Every crash
block prints them:

    the last 32 addresses this thread called indirectly, oldest first.
      0x800838AC          0x80084930 x2       0x800838AC          0x80084930 x2
      0x800838AC          0x80084930 x2       0x800841A4          0x80084930 x4
      0x800841A4 x6       0x80083810          0x800841A4 x2       0x80083810

Three decisions in that, each of which changes what the section is worth:

* **Game addresses, not native ones.** The stack walk above it is a list of
  module offsets that needs a matching `RelWithDebInfo` build to mean anything.
  These need the repository: `grep -rn "glabel func_800838AC" asm/`. A player's
  release-build report now names the neighbourhood in the game's own code
  without the player doing anything at all, which is the whole point.
* **Per-thread.** A fault happens on one thread, and that thread's call history
  is not improved by having three others interleaved into it. It is also
  cheaper: no contention on a path every indirect call takes. A fault on the
  renderer or the event pump has no history and the section is left out.
* **Runs collapsed with a count.** A callback invoked from a loop would
  otherwise fill all 32 slots with itself and the window would cover a
  millisecond. Collapsing costs one compare on the hot path and buys back the
  history behind the loop.

The section is printed by all three crash paths, not only the null call: an
access violation on a game thread is usually the recompiled game dereferencing
something, and what it had been calling is just as much the answer there.

**One `RelWithDebInfo` build**, reproduced locally, with frames 31 to 21 through
`llvm-symbolizer`. Still worth doing, but second now: the ring answers the same
question without needing the build to match.

### Verified

`RAYMAN2_SELFTEST=nullcall-live` trips the null call from inside the running
game -- from the guPerspective hook, on a game thread with a few thousand real
indirect calls behind it -- and produces exactly the report a real occurrence
would produce. The addresses in the sample above are from that run, and all five
distinct ones resolve to labelled functions in `asm/main.s`.

`RAYMAN2_SELFTEST=nullcall` still fires before the game starts, and reports the
empty history as the fact it is: the first indirect call the thread made is the
one that failed.

The intro runs for forty-five seconds at a steady 60-61 display lists a second
with the ring on, matching the rate before it. That is a rate capped by the
frame loop, so it shows the absence of a regression rather than measuring the
cost; the cost per lookup is a compare and two stores beside a hash-map probe
that was already happening.

## Fix, part 3: the hypotheses, tested

### It does not reproduce here

This is the result that should shape everything after it.

`scripts/soak.sh` launches the port, lets it run the intro, kills it, and
tabulates how each run ended. On the **shipped configuration** -- the frontend
build, started through the launcher, which is where all three reports came from
-- twelve runs of twenty-five seconds gave **twelve survivals and no crashes**.
Twelve more on the headless build gave the same.

If the underlying rate were the reporter's two-in-three, the chance of seeing
none in twelve is about two in a million. So whatever trips it is a property of
that machine or that session and not of the build, and no amount of soaking here
will find it. What that changes: the next move is not more local runs, it is
asking the reporter to run the soak with the switches below and send the reports.

### 1. The audio microcode: the mechanism was real, and it is not happening

The hypothesis was that the recompiled audio microcode -- the newest component,
and the only one that writes to RDRAM addresses of its own accord -- was
scribbling on something that later read back as a null pointer.

**The mechanism was real and is now fixed.** `dma_dmem_to_rdram` and
`dma_rdram_to_dmem` bound-checked only the DMEM side, through an `assert` that a
release build compiles away, so in every shipped binary there was no check at
all. `SP_DRAM_ADDR` is a 24-bit register, so a microcode could name a
destination up to 16 MB -- and in this runtime the space between 8 and 16 MB is
not unmapped. `0x80800000` holds the PI handles, `0x80801000` the patch region
and `0x81000000` the mod heap, all committed and writable. A microcode that
computed a destination wrongly would not have faulted; it would have quietly
overwritten the runtime's own bookkeeping and gone wrong somewhere else later,
which is exactly the shape of this bug. Transfers are now clamped to the 8 MB an
N64 can physically have, and one that does not fit says so.

**It never fires.** Not once across every run above.

**And the microcode is not writing anywhere suspicious.** `RSPDMATRACE=1`
reports each distinct 4 KB page a microcode touches. Over a thirty-second intro
the audio microcode writes to exactly eight pages:

    0x8017B000..0x8017DFFF   0x80182000..0x80183FFF   0x80198000..0x8019AFFF

against a segment map whose last code byte is at `0x801012D0`:

    boot  0x80000400 .. 0x8001D0C0
    main  0x80025C50 .. 0x800CDB80
    aux   0x800F64A0 .. 0x801012D0

Eight stable pages, nowhere near code -- the shape of a fixed set of output
buffers, not of wild addresses. Its reads are its own text and data in the boot
segment plus the sample banks above `main`, and a read cannot corrupt anything
regardless.

So the hypothesis is substantially weakened. A wrong-but-in-range write *inside*
those eight pages is not excluded and no bounds check could exclude it, but
there is no longer any evidence for it. `RAYMAN2_NOAUDIOUCODE=1` remains, for
the reporter to A/B on the machine where it happens; the game runs the intro at
a full 60 display lists a second with it set, silently, so the comparison is
clean.

### 2. Scheduling: not settled, and the harness now moves it

`RAYMAN2_YIELD_MS` sets how long a spinning game thread waits for an external
message before yielding -- the port's largest single influence on when the
game's threads run relative to each other, and the thing phase 04's whole bug
class lived in.

    RAYMAN2_YIELD_MS=1 (default)   0 crashes / 12 runs
    RAYMAN2_YIELD_MS=10            0 crashes /  8 runs
    RAYMAN2_YIELD_MS=0 (hot poll)  1 crash   /  8 runs

The hypothesis is untouched by this, because **the one crash is a different
bug**: an `ACCESS_VIOLATION` reading `0x2c4267520dc` -- a reserved, uncommitted
page inside a native heap allocation, not a null pointer -- on a worker thread,
with no `Failed to find function`, no null call and an empty indirect-call
history, meaning the thread had never run recompiled code. It is a genuine fault
and the harness found it, but it is not this one, and it appears only at a
setting the port does not ship. Recorded here so nobody chases it as if it were.

### 3. An early read of a pointer the boot DMA has not filled yet

Untested. There is no cheap switch for it, and the instrumentation from part 2
is the tool: when it happens on the reporter's machine, the call history says
which part of the game was running.

## What to ask the reporter for

    RAYMAN2_AUTOSTART=1 scripts/soak.sh --build build-fe --runs 20

then the same again with `RAYMAN2_NOAUDIOUCODE=1`, and again with
`RAYMAN2_YIELD_MS=10`. Three numbers and the reports behind them settle
hypotheses 1 and 2 between them, and any crash now carries the call history that
names where in the game it happened.

## Fixed on the way

* **The RSP DMA bound**, above: a real hole in every release build, whether or
  not it is this bug.
* **`rayman2::vi_has_ticked()` had no definition in the shipped build.** It is
  declared in `src/main.cpp` and was defined only in `src/rt64_context.cpp`,
  which is compiled *instead of* `src/render_context.cpp` when the frontend is
  off. Nothing in a frontend build referred to it, so it linked. The frontend
  renderer now defines and sets it, from the same place it counts presented
  frames.
