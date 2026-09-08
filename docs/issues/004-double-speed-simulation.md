# 004 — the game runs at double speed; the attract-mode demos show it

**Status:** fixed in `src/frame_pacing.cpp`. The residual inaccuracy is
described under Fix.

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
