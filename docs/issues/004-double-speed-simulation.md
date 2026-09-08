# 004 — the game runs at double speed; the attract-mode demos show it

**Status:** open. Diagnosed, not fixed.

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

Not written. The shape of it is to stop completing the RDP instantly: hold
`dp_complete()` until the frame's deadline, so the game observes a plausible
RDP duration and its own loop limits itself the way it did on hardware.

Two things to be careful of, both of which argue for doing this in the port
rather than reaching for a fixed 30 Hz cap:

* The rate is not a constant. The RDP took as long as the scene needed, so a
  fixed cap would be wrong in both directions -- too fast for a heavy scene, too
  slow for a light one. What the game is entitled to is a *plausible* completion
  time, not a chosen frame rate.
* Delaying the completion delays a message the whole frame handshake is waiting
  on. docs/PHASE04-FINDINGS.md has the record of what happens when that message
  goes missing: one display list, ever. It has to be late, never lost.

## Relationship to issue 003

Unknown, and worth keeping in mind rather than assuming. A loop running at twice
its intended rate has a different thread interleaving from the one the game was
written against, which is the sort of thing an intermittent null pointer comes
out of. Against that: issue 003 reproduces *more* on slow hardware, not less,
and slow hardware is where this defect is smallest. They may well be unrelated.
