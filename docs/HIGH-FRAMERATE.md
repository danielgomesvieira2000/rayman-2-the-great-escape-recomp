# High frame rate, without touching the simulation

RT64 can present more frames than the game draws, by interpolating between two
consecutive display lists: it matches the transforms in one frame against the
next, decomposes each into translation, rotation, scale and skew, and blends
them. That is what `rt64_rigid_body.cpp` and `GameFrame::match` do.

The important property is that **none of it reaches the game**. The extra frames
are generated on the renderer's side of the display list; the game thread is
never told, never rescheduled, and never asked to run faster. Physics, timers
and animation are driven by the game's own clock exactly as before.

This is worth stating precisely, because there is a *different* technique with
the same name in these projects. `ultramodern::get_target_framerate()` exists so
that a patched game can run its own update loop at a higher rate — that one does
change the simulation, and it is the one that needs per-game work to keep
physics correct. This port does not use it and has no such patch.

## What actually had to change

Nothing about the interpolation itself: RT64 matches transforms automatically
when a game does not tag them. `TransformGroup::matrixId` defaults to
`G_EX_ID_AUTO`, and the automatic path matches draw calls between frames by
hashing their contents. The extended-GBI `matrixId` command exists for games
that have been patched to disambiguate difficult cases; an unmodified ROM does
not need it to get interpolation at all.

What has to change is one line, and it is the port's own -- but it is **not** on
by default, for a reason given at the end of this document:

    src/render_context.cpp   PresentationMode::Console -> PresentationMode::SkipBuffering

`PresentQueue::threadPresent` only sets `interpolationEnabled` when the
framebuffer being presented is one the workload modified during this frame:

```
if (colorFb == presentFb) {
    presentFb->interpolationEnabled = true;
}
```

Under `Console` the presented framebuffer is whatever the VI is pointing at,
which for a double-buffered game is the buffer drawn *last* frame, never this
one. The condition is therefore never true, `framesToPresent` stays 1, and no
interpolation happens however the refresh rate is configured. `SkipBuffering`
consults the VI history and presents the buffer that was just drawn, which makes
the condition true and costs a frame of latency less into the bargain.

The refresh-rate plumbing already existed end to end -- the Graphics tab's
option becomes `GraphicsConfig::rr_option`, which recompui hands to RT64 as
`userConfig.refreshRate`, which `WorkloadQueue` turns into `targetRate`:

    Original   targetRate = 0, no interpolation, the game's own rate
    Display    targetRate = the swap chain's rate
    Manual     targetRate = rr_manual_value, clamped down to the swap chain rate

Note the clamp on `Manual`. Asking for 144 on a 60 Hz swap chain gives 60; the
display has to actually be running at the rate being asked for.

## Measured

`RAYMAN2_FPSPROBE=1` reports presented frames per second once a second, together
with the rate RT64 measured from the swap chain.

**Correction.** This section used to say that `RAYMAN2_AUDIOPROBE=1` reports the
rate the *game* is producing audio at, and that comparing the two distinguishes
interpolated frames from a sped-up simulation. It does not, and the claim was
asserted rather than checked. The AI message is enqueued once per VI retrace at
`ultramodern/src/events.cpp:263`, unconditionally and with no reference to the
game's update loop, so audio synthesis is paced by the 60 Hz tick and the
22050 Hz sample rate. A game loop running at twice its intended rate produces
the same number of samples per second, and the probe reads identically either
way -- measured over three minutes in docs/issues/004, where the simulation
*is* running at double speed and the audio rate does not move.

So the audio probe answers "is the audio path keeping up", which is what it was
built for. It does not answer "is the simulation running at the right speed",
and there is currently no probe that does; the honest test is to time a known
sequence -- an attract-mode demo -- against a console or an accurate emulator.

On the development machine, whose panel is 60 Hz:

    Console        presented 47-60 frames/s   audio 22400 frames/s
    SkipBuffering  presented 60.0 frames/s    audio 22400 frames/s

The dips to 47 under `Console` were the double-buffered presentation, not the
game: with interpolation explicitly disabled (`RefreshRate::Original`)
`SkipBuffering` still holds 57-60. So on a 60 Hz display, where the port is
already producing about 60 frames a second, what this change buys is a steady
presented rate rather than a higher one.

That the port produces 60 was read here as the game reaching its own rate. It is
not: docs/issues/004 establishes that the game's loop is paced by RDP
completion, which this runtime signals instantly, so 60 is the *defect* rather
than the game's rate. That does not change anything above about interpolation --
none of it reaches the game either way -- but it does mean the measurements in
this section were taken against a simulation running at roughly twice its
intended speed, and should be retaken once 004 is fixed.

The interpolation proper cannot be demonstrated on a 60 Hz panel while the port
is already presenting 60. To see it, run on a display above 60 Hz with the
refresh rate option set to `Display`, and check that `RAYMAN2_FPSPROBE` reports
the panel's rate. There is no second probe to check the simulation against; see
the correction above.

If the presented rate stays at the game's own rate on such a display, the next
thing to try is `PresentationMode::PresentEarly`, which submits the presentation
event as soon as the display list is finished and so guarantees the condition
above rather than relying on the VI history.

## Why it is off by default: torn frames

Presenting the buffer the game has just drawn also means presenting it while the
game may still be drawing into it, and on Rayman 2 that is visible. Captured
frames under `SkipBuffering` had whole regions of the scene missing and the HUD
digits sliced off; the same scene under `Console` is complete every time. It is
neither subtle nor rare, so it cannot be the default, and the release ships as
`Console`.

That is the real state of this work: the mechanism is understood and reachable,
the physics question is settled, and what remains is a rendering defect in the
mode that unlocks it. Whether it is a Rayman 2 quirk, an RT64 issue with this
game's framebuffer usage, or something the port is doing to the VI is the next
thing to find out.

Both modes stay available without a rebuild, because the thing they unlock
cannot be evaluated on a 60 Hz display by a game that already reaches 60:

    RAYMAN2_PRESENT=skipbuffering
    RAYMAN2_PRESENT=presentearly

Anything else, including leaving it unset, is `Console`.
