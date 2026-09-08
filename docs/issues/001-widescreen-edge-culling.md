# 001 — Geometry culled at the edges in widescreen

**Status:** open, mechanism established, not yet reproduced against a specific
place. Needs a capture.

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
