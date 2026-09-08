# 001 — Geometry culled at the edges in widescreen

**Status:** open. Reproduced, the lever found and proven, and one blocker
identified. The remaining step is a decision about what the Aspect Ratio menu
should mean, not more investigation.

---

## What is wrong

Reported from playtesting as culling artefacts "in some places". Not yet
captured, so what follows is the mechanism the code makes inevitable rather than
a description of the observed defect. The two need to be joined up before
anything is changed — see *What is needed next*.

The expected symptom is scenery appearing and disappearing in the left and right
strips of a widened frame: the parts of the picture a 4:3 screen would not have
shown. It should be absent entirely with the aspect ratio set to Original.

## The mechanism

Established by reading, and not in doubt:

**RT64 never culls display lists.** `cullDl` in `rt64_gbi_f3d.cpp` is an empty
function with a `// TODO` in it, for F3D and for F3DEX. Whatever the game
submits, RT64 draws. So the renderer cannot be removing this geometry and no fix
belongs there.

**RT64 widens the view without telling the game.** With `ar_option = Expand`,
`WorkloadQueue` computes

    aspectRatioScale = aspectRatioTarget / aspectRatioSource

and applies it to the projection (`projParams.aspectRatioScale`) and to the
viewport and scissor rectangles (`convertFixedRect` in
`rt64_framebuffer_renderer.cpp`). The widening happens entirely on the
renderer's side of the display list.

**So the game still believes it is drawing 4:3.** Rayman 2 decides on the CPU
which objects are worth submitting, using its own frustum. Anything it rejects
because it falls outside a 4:3 view is never submitted, never reaches RT64, and
therefore cannot appear in the strips RT64 has just made visible. The wider the
frame, the more of the picture is drawn from geometry the game has already
decided the player cannot see.

This is the same defect the sibling Beetle Adventure Racing port describes
fixing — "the culling done against the widened view so scenery no longer pops
out at the sides of a widescreen frame" — so the shape of the answer is known
even though the location in this game is not.

## Where the fix has to go

In the game, not the renderer. Something in the cartridge's own code computes a
horizontal extent for its visibility test, and that extent has to be widened by
the aspect ratio actually in force, which the port can read from
`ultramodern::renderer::get_graphics_config()`.

Two things make this harder than it sounds, and both argue for reproducing it
first:

  * There is no decompilation to donate names. The routine is one of about 4,500
    functions currently called `func_XXXXXXXX`, and it has to be found from its
    behaviour.
  * The obvious search did not hit. The ROM contains no `1.3333334` and no
    `0.75`-as-an-aspect constant in any position that looks like a projection
    setup, so the frustum is probably built from a field of view and a viewport
    size rather than from a stored ratio. That rules out the cheapest way in.

## What was ruled out

  * **RT64 display-list culling.** `cullDl` is a no-op; not the cause.
  * **The clip ratio.** F3DEX's `gSPClipRatio` reaches RT64 through
    `G_MW_CLIP` -> `RSP::setClipRatioEdge`, but RT64 uses those values for the
    viewport rectangle rather than to reject geometry, and that rectangle is
    already widened by `aspectRatioScale`. Not the cause.
  * **The scissor.** Already widened, for the same reason. If it were not, the
    widened strips would be blank rather than sparse, and widescreen would
    plainly not work — and it does.

## What is needed next

A capture at one of the places where it was noticed. Press **F9** while it is on
screen and attach the folder from `debug-report/captures/`, plus the answer to
one question:

> With the aspect ratio set to **Original** instead of Expand, at the same spot,
> is the defect still there?

That single answer decides everything. If it disappears, this is confirmed and
the hunt is for the game's frustum. If it survives, this issue is the wrong
explanation for what was seen — the symptom is something else wearing the same
clothes, and building the widening would have fixed nothing while looking like
progress.

A Controller Pak image parked just before the spot would make the difference
between chasing it once and chasing it every time it needs re-testing.

## Investigation

*2026-09-08* — Mechanism established as above. Attempted to reproduce by
capturing the same scene under Expand and Original: inconclusive, because the
automated runs did not land on the same save state or the same camera, and with
a 4:3 window the Expand image is letterboxed rather than widened, so the strips
that matter were not on screen. Reproducing this properly wants a person at the
controls and a wide window.

Deliberately not fixed on the strength of the mechanism alone. A change to the
game's culling that cannot be checked against the symptom it is supposed to
remove is a guess with a commit message attached, and this project has already
paid once this week for shipping a rendering change that was measured but not
looked at.

---

## Reproduced

The opening cinematic, where the camera flies past mountains over the ocean, on
the subtitle "THEY'VE TAKEN EVERYTHING AND REDUCED OUR PEOPLE TO SLAVES". A hard
vertical seam appears near the right of a 1920-wide frame: the scene stops, and
what is beyond it is wrong.

The position is the confirmation. The game renders at an aspect of 1.3393, RT64
expands to 1.7778, so the game's own view occupies 1.3393/1.7778 = 75.3% of the
frame -- centred, that is x = 236 to x = 1683. The seam sits at the right end of
that, which is precisely where the game stops believing anything is visible.

It reaches itself: boot the port with a save already stored and the cinematic
plays with no input at all, so this is reproducible without a controller.

## The lever, proven

`func_800038C0` is `guPerspective` (see [002](002-draw-distance.md)) and its
fourth argument is the aspect ratio, arriving as 1.3393 -- which is 300/224, the
framebuffer this game renders.

Scaling that argument was tested at x2 against a frame-matched moment (the same
subtitle, "THE ROBOTS SEARCH FOR INNOCENT PREY", in both runs). At x2 the same
two mountains are drawn at roughly half their width with sea and sky around
them, and the subtitle text is narrower. The horizontal field of view had
doubled.

So the game's frustum -- and whatever culling derives from it -- is downstream of
that one argument. This is the same arrangement Beetle Adventure Racing
describes: one set of numbers decides both what the projection draws and what
the game bothers to submit.

## Why the one-line fix does not work

Widening the game's aspect widens the *rendered* view as well, and RT64 is
already widening it. The two multiply, and the result is a view about a third
too wide rather than a correctly culled one.

The fix therefore has to move the widening rather than add to it: give the game
the display's aspect, and stop RT64 expanding on top. The game then renders a
wide view squeezed into its 4:3 framebuffer -- anamorphic, exactly as widescreen
hacks for real N64 hardware do it -- and the presentation stretches it back out.

## The blocker

That configuration cannot currently be set, and the reason is a bug in its own
right.

`ar_option = Original` plus `pfm_option = Stretch` should do it. Setting both in
`graphics.json` produced a pillarboxed 4:3 image: `pfm_option` was ignored.

The cause is in the frontend. `apply_graphics_config()` builds a
default-constructed `GraphicsConfig` and assigns only the options the graphics
tab knows about, so any field the fork added and the tab does not list -- which
is `pfm_option` and `divot_option` -- is silently reset to its default every time
the configuration is applied. The fork's own comment beside those fields warns
about exactly this and is the reason they carry default initialisers; what it
does not prevent is the value being *overwritten* rather than left
indeterminate.

So `pfm_option` has no effect from the configuration file, and the anamorphic
route cannot be tested until the port sets it itself.

## What is left to decide

Not an investigation -- a design choice, because the fix changes what the Aspect
Ratio menu means. Once the game is the thing producing the widescreen view,
RT64's Expand has to be off, and the menu option that currently reads "Expand"
would have to drive the game's aspect instead of the renderer's.

That is a small amount of code in the port and no change to the submodule. It
wants agreeing before it is written, because it moves a user-facing setting.
