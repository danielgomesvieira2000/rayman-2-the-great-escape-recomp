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

What had to change was one line, and it was the port's own:

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
with the rate RT64 measured from the swap chain. Read it against
`RAYMAN2_AUDIOPROBE=1`, which reports the rate the *game* is producing audio at:

  * if the presented rate rises while the audio rate does not, the extra frames
    are interpolated and the simulation is running at its original speed;
  * if both rise, something has sped the game up, and the physics with it.

On the development machine, whose panel is 60 Hz:

    Console        presented 47-60 frames/s   audio 22400 frames/s
    SkipBuffering  presented 60.0 frames/s    audio 22400 frames/s

The dips to 47 under `Console` were the double-buffered presentation, not the
game: with interpolation explicitly disabled (`RefreshRate::Original`)
`SkipBuffering` still holds 57-60. So on a 60 Hz display, where the game already
produces about 60 frames a second, what this change buys is a steady presented
rate rather than a higher one. The audio rate is identical in every case, which
is the measurement that matters: the game is running at the same speed it always
did.

The interpolation proper cannot be demonstrated on a 60 Hz panel by a game that
already reaches 60. To see it, run on a display above 60 Hz with the refresh
rate option set to `Display`, and check that `RAYMAN2_FPSPROBE` reports the
panel's rate while `RAYMAN2_AUDIOPROBE` still reports about 22400.

If the presented rate stays at the game's own rate on such a display, the next
thing to try is `PresentationMode::PresentEarly` in `src/render_context.cpp`,
which submits the presentation event as soon as the display list is finished and
so guarantees the condition above rather than relying on the VI history.
