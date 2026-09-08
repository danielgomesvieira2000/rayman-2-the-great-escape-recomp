# 002 — Draw distance: the far plane is not what limits it

**Status:** investigated; the requested option is not worth shipping as asked.
The mechanism it would have used is in place, proven, and off.

---

## What was asked for

A Draw Distance option in the Graphics tab, at 1x, 1.5x, 2x and 4x — matching
the sibling Beetle Adventure Racing port, which has one.

## What was found

`func_800038C0` is libultra's `guPerspective`, identified from its arithmetic
rather than from any call site: fovy multiplied by a **double-precision** PI/360
(which is why searching the ROM for a single-precision PI constant finds
nothing), sine over cosine for the tangent, and the projection's depth terms
built as `(n+f)/(n-f)` and `2nf/(n-f)` with a `-1.0f` beside them. It has two
call sites, one inside `func_8009989C` in the main segment — the game's camera
code. `func_80003710` beside it is `guPerspectiveF`, which nothing calls.

A hook at `at_func_start` reads its arguments off the caller's stack, and
`RAYMAN2_DDPROBE=1` prints what it sees:

    guPerspective fovy=69.644 aspect=1.3393 near=32.000 far=8192.000
    guPerspective fovy=54.118 aspect=1.3393 near=64.000 far=32768.000

Two things are settled by those four numbers.

**The identification is certain.** `aspect = 1.3393` is not 4:3 as a round
number — it is 300/224, exactly the framebuffer dimensions phase 00 measured for
this game. Arguments read at the wrong stack offsets do not accidentally produce
the framebuffer's own aspect ratio.

**And the feature is pointless.** The far plane is already 8192 units against a
near plane of 32, and 32768 against 64. Those are not draw distances; they are
"effectively infinite". Nothing in Rayman 2 is being hidden by the projection's
far plane, so multiplying it by four changes what the projection would keep
without changing anything the player can see. Two screenshots at 1x and 4x
differ only in where the camera happened to be.

That is the difference from Beetle Adventure Racing, whose racing camera has
`far = 300` — a genuinely tight far plane, where scaling it does exactly what
the label says.

## Why the option is not being shipped

An option labelled "Draw Distance 4x" that multiplies a number nothing is
bounded by would be a menu entry that does nothing, discoverable only by a
player concluding the port is broken. Shipping it would be worse than not having
it.

## Where Rayman 2's draw distance actually lives

Not established. On the evidence it is one of, or both of:

  * **Sector and portal visibility.** Rayman 2's engine is a sector engine; what
    is drawn is decided by which sectors are reachable from the camera's, long
    before any frustum test. Extending that is a much larger change than a
    multiplier, and it risks drawing geometry the level design assumes is
    invisible.
  * **Fog.** The game fades distant geometry into fog; the fog end distance, not
    the far plane, is what the eye reads as draw distance. Pushing fog back is
    plausible and cheap, and is a different feature from the one asked for.

Either is a real piece of work and both need a place to look at, which is what
phase 07's playthrough will produce.

## What was kept

`src/draw_distance.cpp` and the `guPerspective` hook stay in the tree, at 1x by
default and doing nothing unless `RAYMAN2_DRAWDIST` asks. They are worth keeping
for two reasons beyond this issue:

  * The identification of `guPerspective` and its call sites is a fixed point in
    a game with no decompilation, and the probe is how it was established.
  * `aspect` is right there in the same call, at 1.3393, which is the most
    promising lever yet found for
    [001](001-widescreen-edge-culling.md) — *if* the game's culling is derived
    from the same projection. That is the next thing to test, and it now costs a
    one-line change rather than a search.
