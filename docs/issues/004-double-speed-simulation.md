# 004 — the game runs at double speed; the attract-mode demos show it

**Status:** open, and shipping unfixed by decision. The demos run at double
speed; everything else is correct and smooth. The cap that fixes the demos costs
the rest of the game half its frames, and the attempt to gate it on the demos
failed -- see "The gate did not work". `RAYMAN2_FRAMECAP=30` re-enables the
unconditional cap for anyone who would rather have it.

## What is wrong

Left alone at the title screen, the game eventually plays its attract-mode
demos. They run at about twice the speed they should.

The demos are where it is visible, not where it happens. A demo is a recorded
input sequence played back against the game's own update loop, so it is the one
thing in the game whose correct duration a player already knows. Everything else
-- the intro cinematic, the menus, play itself -- is running at the same rate,
and nothing in it has a remembered length to be compared against.

## Where

The title screen. Wait, and the demo starts on its own.

## The mechanism

Rayman 2's frame loop is a strict handshake, and this port already had the
description of it written down, in the message-queue note in `src/main.cpp`:

> the producer takes the single free framebuffer slot, builds a display list and
> posts it; the task thread submits it; the RDP finishes; the completion message
> returns the slot.

So the loop advances exactly as fast as RDP completion comes back to it. **The
game is not paced by the video interrupt; it is paced by how long the RDP takes
to draw.** That is an ordinary way for an N64 game to end up frame-limited, and
it is why Rayman 2 does not hold 60 on a console: the RDP takes longer than one
field to draw one of its frames, so the loop gets a turn roughly every other
field.

`lib/N64ModernRuntime/ultramodern/src/events.cpp:391` completes it instantly:

    renderer_context->send_dl(&task_action->task);
    dp_complete();

`dp_complete()` is called on the line after `send_dl()` returns -- the moment
RT64 has *accepted* the display list, with no emulated drawing time at all. On a
modern GPU that is microseconds. The slot comes straight back, the producer
never waits, and the loop runs at the full VI rate of 60 Hz instead of the
roughly 30 the hardware allowed.

Double the update rate is double the simulation speed, which is what the demos
are showing.

## What rules out the other candidates

* **The VI retrace count is honoured.** `events.cpp:259` reloads
  `remaining_retraces` from `cur_state->retrace_count`, so a game asking for a
  message every second field gets one every second field. Retraces are not being
  over-delivered.
* **The VI thread's own pacing is right.** `events.cpp:206` targets
  `total_vis / 60`, which is the NTSC field rate.
* **It is not the frame interpolation.** That is off by default
  (`PresentationMode::Console`), and it never reaches the game anyway --
  see [HIGH-FRAMERATE.md](../HIGH-FRAMERATE.md).
* **It is not `get_target_framerate`.** That exists for a game patched to run
  its own update loop faster; this port does not use it and has no such patch.

## The measurement, and a correction to the tool

`RAYMAN2_FPSPROBE=1` over three minutes at the title screen, through several
demo cycles:

    title screen   presented  2.3 frames/s   audio peak 0
    demo playing   presented 60.0 frames/s   audio peak 9000-23000

The demo half is a **pegged** 60.0, not a rate that moves with what is on
screen. That is the signature of nothing limiting the game rather than of a game
that happens to reach 60.

`RAYMAN2_AUDIOPROBE=1` reported a flat 21920-23040 frames/s throughout, in both
halves. **That does not clear the simulation, and HIGH-FRAMERATE.md is wrong to
say it would.** The AI message is enqueued once per retrace at
`events.cpp:263`, unconditionally and independently of the game's update loop,
so audio synthesis is paced by the 60 Hz tick and the 22050 Hz sample rate. A
game loop running at twice the rate does not produce twice the samples, and the
probe cannot see it. The claim has been corrected in that document.

## Still to confirm

Time one demo in the port against the same demo on a console or an accurate
emulator. If ours is half the length, the diagnosis is complete. This is the one
thing here that has not been measured rather than reasoned, and it is cheap.

## Correction: only the demo is fast, and a global cap is the wrong trade

**The diagnosis below is half right, and the half that is wrong is the half
that decided what to do about it.**

That the game's loop is paced by RDP completion, and that this runtime completes
it instantly, is established and unchanged. What was inferred from it -- that
the whole game therefore runs at twice its intended speed -- was never measured.
Everything in the measurements below is the intro cinematic and the attract
loop, because a scripted soak has no controller and cannot get past them.

Playtesting says gameplay runs at the **correct** speed uncapped, and only the
attract-mode demos are fast. That fits a game whose physics advance on elapsed
time -- and so come out right at any frame rate -- while demo playback is
frame-indexed, one recorded input per frame, and therefore doubles exactly when
the frame rate does. It also explains why the demos were the only place anyone
noticed: they are not merely the sequence with a known duration, they are the
part of the game that is actually wrong.

So a global cap trades every frame in the game for a cosmetic defect in attract
mode. That is a bad trade and it was rejected as one: **the cap ships off.**
With no pacer installed ultramodern completes the RDP exactly as it always did,
so "off" costs not even an indirect call.

### The flag, and how it was found

`src/demo_scan.cpp`, run unattended under `RAYMAN2_DEMOSCAN=1`. The two states
are separable from outside without knowing anything about the game -- the title
screen submits about two display lists a second because it is barely redrawing,
a demo submits sixty -- so the port can label its own samples, and the flag is
whatever word is reliably one value under one label and another under the other.

An ordinary cheat search, with the labelling automated. The automation is the
point: the attract loop alternates on its own every twenty seconds or so for as
long as the game is left alone, so an unattended run gets a dozen transitions,
where a person watching a memory viewer gets one per attempt and has to catch
it. Over five cycles, 2,097,152 words narrowed to three:

    603947 -> 45500 -> 400 -> 236 -> 147 -> 141 -> 119 -> 115 -> 101 -> 3

    0x800E4B18   title 0x00000003   attract 0x800CE258
    0x800E4B1C   title 0x00000001   attract 0x00000005
    0x800E4B20   title 0x00000000   attract 0x00000003

The first is a RAM pointer while a demo plays and a small integer otherwise --
what a pointer to the recorded input stream would look like. The other two read
as a mode enum. They are adjacent, so this is one small structure rather than
three coincidences.

`attract_mode_active` requires **all three** to match. Any one alone is a
plausible flag and a plausible coincidence; three adjacent words agreeing is
neither. The cost of a false positive is capping the frame rate during play,
which is the outcome the whole mechanism exists to avoid, so the test is as
specific as the evidence allows rather than as cheap as possible.

Confirmed by watching the three through the attract loop with the display-list
rate beside them (`RAYMAN2_WATCH`): the two value sets separate 0-22 lists a
second from 54-62 with no overlap, and the only off-pattern samples are single
transition windows where the half-second rate average lags the flag. The flag
leads the rate, which is what a real state variable does and what an incidental
correlate would not.

### Measured, gated

Seven engage/release cycles over two minutes, at the demo boundaries and nowhere
else:

    attract-mode demo started: frame cap engaged
    attract-mode demo ended: frame cap released

and during the demos, pace 2.00-2.00 fields with present 2f:150, spread
1.88-2.11 -- the cap exact and the picture even. Outside them nothing is paced
at all.

### The gate did not work

Playtesting: with the cap gated on that signature, **everything ran at half
speed**. So `attract_mode_active` is true during gameplay as well, and what the
search actually found is a flag that separates the title screen from *anything
the engine is running* -- which includes a demo and includes play. A real
distinction, and not the one needed.

In hindsight the pointer is the tell. `0x800CE258` is far more likely the
current scene or level than a recorded input stream, and a scene is loaded
whoever is driving it. The reading in the section above -- "what a pointer to
the recorded input stream would look like" -- was a story that fitted the two
samples available and was not evidence for anything.

The failure was predicted and cost one playtest rather than a release, because
the gate logged every time it engaged and the claim it rested on was written
down as an inference rather than a result. That is the part worth keeping.

**Where the next attempt should look.** Not in this structure. The demo-versus-
gameplay distinction has to be something that differs between two states that
both have a level loaded, and the display-list rate cannot label those -- both
run at sixty -- so the automated search that found this one cannot find that
one. Labelling would have to come from somewhere else: a person driving the game
and marking the states by hand, or a signal like whether the controller is being
read at all.

### What was taken on trust, and was wrong

The search could only label the two states the port can tell apart from
outside. **Gameplay was never sampled**, because a scripted run has no
controller and cannot reach it, so the signature is known to separate the title
screen from a demo and is *inferred* to separate a demo from play.

That is why every engage and release was logged -- and it is how the failure
above was caught in one session rather than in a release.

### What fixing the demo properly would need

(Kept as the record of what the choice was.) Two routes:

* **A game-side flag.** Find the byte that says the game is in attract mode and
  read it. Reliable once found, and finding it is a search: sample RDRAM while
  the title screen is up and again while a demo runs -- the two are trivially
  distinguishable by display-list rate, about 2.3 a second against 60 -- and
  keep the addresses that are consistently one value in one state and another
  value in the other.
* **Inference from the port's own side.** A demo runs with no player input,
  after the title screen, and ends when somebody presses Start. All three are
  visible to the port. It needs no reverse engineering and it is a heuristic:
  the intro cinematic looks the same from outside, and a wrong guess caps
  gameplay, which is the one outcome this correction exists to prevent.

The first was taken, and is described above. The second was not needed.

### What is kept

The machinery, the probes and the measurements, because they are correct and
because the demo is still wrong. `RAYMAN2_FRAMECAP=30` re-enables the cap and
reproduces everything described below, including the judder work, which was a
real defect in the pacing and is fixed.

## Fix

`src/frame_pacing.cpp`, called at the end of `send_dl` in both renderer
wrappers. That is the last thing that runs before ultramodern signals
`dp_complete()`, so the port can delay the completion without touching the
runtime at all -- and delaying there is faithful to the mechanism: RT64 is
handed the work immediately and only the completion signal waits, which is
exactly what an RDP that is still busy looks like from the game's side.

It is in **both** renderers on purpose. `src/render_context.cpp` is built with
the frontend and `src/rt64_context.cpp` without it, and a fix in one with the
measurements taken against the other is how this defect stayed invisible in the
first place.

Sleeping on that thread costs nothing that was being used: it exists to carry
graphics tasks, audio tasks are dispatched from a different queue on a different
thread -- `events.cpp` enqueues an `SpTaskAction` only for `M_GFXTASK` -- and
the game itself is blocked waiting for precisely the completion being delayed.
The completion is made **late, never lost**, which is the caution
docs/PHASE04-FINDINGS.md paid for: a dropped one gives one display list, ever.

### The cap has to land on the VI's grid, not on a timer of its own

The first version of this slept until `previous + 1/30s`. The average rate was
right and the picture juddered -- reported from play as "back and forth
jittering", not merely as a lower frame rate.

A schedule of its own is the mistake. The video interrupt puts field *k* at
`get_start() + k/60` (`events.cpp`), and a frame completing on any other
schedule sits at an arbitrary phase against those boundaries. Sleep granularity
is about a millisecond, so a phase that happens to sit near a boundary sends
consecutive frames to either side of it at random, and they are shown for one
field or three instead of two. Whether a run looked smooth then depended on
where the first frame happened to fall, and every resync after a missed deadline
re-rolled it.

`ultramodern::get_start()` and `get_speed_multiplier()` are public, so the port
can compute the same grid and snap each completion onto it. The interval is then
exactly two fields every time and the phase is constant for the whole run.

The knob is expressed in **fields per frame** internally for the same reason: a
rate that is not a divisor of 60 cannot be delivered evenly on this grid.
`RAYMAN2_FRAMECAP` is still given in frames a second because that is what a
person means, and the achieved rate is reported when it is set, so asking for 45
and being given 30 is visible rather than mysterious.

### Measuring judder, which a rate cannot show

Thirty frames delivered 2,2,2,2... and thirty delivered 1,3,1,3... are both
"30 a second" and only one of them looks right, so the per-second rate is blind
to exactly the defect above. `RAYMAN2_PACEPROBE=1` reports the measured gap
between consecutive frames in VI fields, bucketed, every five seconds. A cap
that is working reads as a single bucket:

    pace: 2f:150   spread 1.90-2.11 fields

Over fifty seconds, 590 of 594 frames were delivered at exactly two fields. The
four that were not are the first frame, the fifteen-second stall below, and the
resync after it.

### It is a cap, not a model of the RDP

This is the honest limitation. A real RDP took as long as the scene needed; no
cap reproduces that. A heavy scene ran slower than 30 on hardware and still runs
at 30 here, so the residual error is in the direction of running slightly too
fast in the heaviest scenes -- against the factor of two it replaces. Modelling
the real thing would mean knowing how long each display list would have taken on
an RDP, which the port does not know and RT64 does not report.

Thirty because that is what the defect measures: exactly twice intended, against
a 60 Hz field rate. `RAYMAN2_FRAMECAP=<n>` overrides it and `RAYMAN2_FRAMECAP=0`
turns it off, so the comparison can be repeated without a rebuild.

### Measured

The headless build reports the game's own display-list rate, which is the
quantity that was wrong -- one display list is one iteration of the game's loop:

    RAYMAN2_FRAMECAP=0    display lists  +61/s   (the defect)
    RAYMAN2_FRAMECAP=30   display lists  +30/s   steady, 30-31 across the run

VI is unchanged at 60-61/s in both, which is the check that the field rate was
not what moved.

End to end in the shipped configuration, through several attract-mode demo
cycles, presented frames a second while drawing:

    before   a pegged 60.0
    after    30.0 in 47 of 74 samples

The samples that are not 30.0 are the seconds that straddle a demo starting or
ending, which contain some idle title screen and some demo.

One pre-existing pause was checked rather than assumed: the intro contains a
stretch where the game submits nothing for fifteen seconds. It is fifteen
seconds with the cap and fifteen without, so the cap does not lengthen it. It is
not understood, and it is not this issue.

### Still worth doing

Time a demo against a console or an accurate emulator. The cap is derived from
the reported factor of two rather than from a reference recording, so the
factor is confirmed but the absolute rate is not.

## Relationship to issue 003

Unknown, and worth keeping in mind rather than assuming. A loop running at twice
its intended rate has a different thread interleaving from the one the game was
written against, which is the sort of thing an intermittent null pointer comes
out of. Against that: issue 003 reproduces *more* on slow hardware, not less,
and slow hardware is where this defect is smallest. They may well be unrelated.
