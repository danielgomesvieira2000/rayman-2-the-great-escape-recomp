# 001 — Geometry culled at the edges in widescreen

**Status:** open. Four attempts, all failed, and the fourth rules out the whole
approach: `guPerspective` is not where this can be fixed. What is left is
finding what Rayman 2's visibility test actually reads, which has not been
attempted and should be done by looking rather than by inference.

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

## The fix, written and gated

`rayman2::update_widescreen_policy()` in `src/draw_distance.cpp` implements the
repurposing: when the player asks for Expand it hands the game the display's
aspect through the `guPerspective` hook and tells RT64 not to expand, so the
widening moves from the renderer to the game and the game's culling follows it.

Two details make it work without forking the frontend:

  * It runs every frame rather than once, because `apply_graphics_config()`
    rebuilds the configuration from a default-constructed `GraphicsConfig` each
    time any setting is applied, and would undo a single push.
  * The menu goes on showing what the player chose. The tab renders from the
    frontend's own option store and `apply_graphics_config` only ever pushes
    from there into ultramodern; nothing reads back. Rewriting ultramodern's
    copy is invisible to it.

It is **off by default**, behind `RAYMAN2_WIDESCREEN=game`.

## The blocker, properly identified

The plan needs the presentation to stretch the game's 4:3 framebuffer across the
window. `PresentFillMode::Stretch` is exactly that setting, and it does nothing
at all, because **nothing reads `pfm_option`**.

The field was added to `GraphicsConfig` in the N64ModernRuntime fork. It is
serialised, it carries a default initialiser, and there is a long comment beside
it explaining which fill mode should be the baseline and why. But the code that
configures RT64 is `set_application_user_config()` in recompui, which is
upstream and knows nothing about the field, and no other reader exists anywhere
in ultramodern or the port. Setting it in `graphics.json`, or from the menu, or
from this policy, has never had any effect.

That is why enabling the policy today produces a *pillarboxed* frame: RT64 stops
expanding, as intended, and then nothing tells the presentation to fill the
window. Correct culling in a smaller picture is not a trade worth making
silently, so it stays gated.

This also affects the sibling Beetle Adventure Racing port, which added the
field and describes its letterbox behaviour as a baseline to be moved off later.

## What unblocks it

One of:

  * **Plumb `pfm_option` through to RT64.** RT64's own knob is
    `EnhancementConfiguration::Presentation::removeBlackBorders`, which is
    exposed in its developer UI as "Remove Black Borders". Wiring the existing
    config field to it belongs in recompui and is a change worth sending
    upstream rather than forking for.
  * **Or drop the anamorphic route and keep RT64's Expand**, widening the
    game's frustum for culling only -- which needs the projection matrix
    written back to its un-widened form after `guPerspective` builds it. That
    works only if the game culls from the camera rather than from the matrix it
    just produced, which is not yet known and is one more experiment.

## The fix as shipped

The anamorphic route was abandoned: it produced a widescreen render displayed in
a 4:3 window, which is a worse picture than the one it replaced. The picture has
to *be* widescreen.

So RT64 keeps doing exactly what it does today -- Expand, filling the window --
and only the game's frustum changes. Two hooks on `guPerspective`:

  * **on the way in**, the aspect argument is replaced with the display's, so
    everything the game derives from its frustum, culling included, is computed
    against the frame the player is actually looking at;
  * **on the way out**, the projection matrix's `[0][0]` is multiplied back, so
    the matrix handed to the RSP is the one an unmodified call would have
    produced and RT64's own widening lands on top of it exactly as before.

`[0][0]` is the only aspect-dependent term guPerspective writes; `perspNorm`
comes from near and far alone.

Verified with `RAYMAN2_DDPROBE=1`:

    guPerspective fovy=69.644 aspect=1.7778 near=32.000 far=8192.000
    mtx 0x800E5F38 [0][0] 0.80865 -> 1.07341 (k=1.3274)

1.3393 became 1.7778, the display aspect, and 0.80865 -- which is
cot(fovy/2)/1.7778 -- was restored to 1.07341, which is cot(fovy/2)/1.3393. That
is the un-widened value to five decimal places.

Also gone: the `PresentFillMode` blocker no longer applies, because nothing now
depends on the presentation stretching anything.

## Still to confirm

That the framing is identical to before. The numbers say it must be, and the
intro renders full-width with no seam and both mountains whole -- but the
cinematic camera moves continuously, so captures taken four seconds apart in two
runs land at different points within the same subtitle, and apparent size cannot
be compared between them. It is obvious to a person playing: if the field of
view looks unchanged from the last build, it is right.

## The frustum fix does not work, and why that is useful

Playtesting: *"Mountains disappear after the 4:3 area mark, but I see them going
too soon."* The cull boundary did not move.

That is decisive, and it settles something no amount of reading could. The pair
of hooks widened the aspect on the way in and restored the matrix's `[0][0]` on
the way out. If the game's visibility test were computed from the arguments --
from a camera struct holding fovy and aspect -- widening those would have moved
the boundary and restoring the matrix would not have mattered. It did not move.

**So the game culls from the projection matrix it just built.** Restoring
`[0][0]` restores the narrow frustum for the culling as well, and the two hooks
cancel exactly. They are disabled; `RAYMAN2_WIDESCREEN=frustum` re-enables them
for experiments.

## Which leaves exactly one arrangement

The matrix is a single number serving two purposes: what the game keeps and what
the renderer draws. They cannot be given different values. So:

  * the matrix must **be** the widened one -- that is what fixes the culling;
  * and RT64 must then **not** widen on top of it, or the view comes out a third
    too wide and the seam simply moves outward;
  * which means RT64 renders at the game's own 4:3 proportions, and the
    presentation has to fill the window without applying an aspect scale.

That last step is `PresentFillMode::Stretch`, and it is the dead field: added to
`GraphicsConfig` in the fork, serialised, documented, and read by nothing.
recompui's `set_application_user_config()` is what configures RT64 and has never
heard of it. RT64's own `presentation.removeBlackBorders` is already true by
default and is a different thing -- it removes borders the *game* draws, not the
ones the window fit produces.

## What it needs

One small change in recompui: pass `pfm_option` through to RT64's presentation
so the fill mode can be selected. Everything else -- the widening, the policy,
the hook -- is already written and proven.

That is a submodule, and this project's standing constraint is that the frontend
is consumed rather than forked, with changes that belong upstream going
upstream. This one plainly belongs upstream: a configuration field that exists,
is saved to disk, and is silently ignored is a bug in RecompFrontend regardless
of what this port wants from it. It also affects the sibling Beetle port, whose
letterbox behaviour is documented as a baseline to be moved off later and
currently cannot be.

So it wants a decision before it is written: fork the submodule and carry a
patch, or send it upstream and wait.

## The recompui patch has nothing to attach to

Before forking RecompFrontend to plumb `pfm_option` through, the destination was
checked. There is no destination.

`RT64::UserConfiguration` has no fill, stretch or letterbox option of any kind.
Its whole aspect model is `AspectRatio` -- Original, Expand or Manual, plus
`aspectTarget` -- and every one of those works by scaling the *projection*:

    aspectRatioSource = the game's framebuffer aspect, from the VI
    aspectRatioTarget = what is wanted on screen
    aspectRatioScale  = target / source, applied to the projection

RT64 widens the field of view and renders a correspondingly wider target, which
then fills the window. It never stretches a 4:3 image. So `PresentFillMode`
describes a capability RT64 does not expose, and passing `pfm_option` through
recompui would be passing it to nothing. The field is not merely unplumbed; it
was speculative.

That kills the anamorphic plan outright rather than blocking it, and it is worth
having found before opening a pull request that could not have worked.

## The route that does follow from the evidence

Everything measured so far says:

  * the game culls from the projection matrix;
  * RT64 needs that same matrix to stay narrow, because its Expand multiplies it
    to produce the displayed field of view.

Both can be true at once, because they read the matrix at *different times*. The
game culls while it builds its frame; RT64 reads the matrix later, when it
processes the display list. So leave the wide matrix in place for the game --
drop the restore at `guPerspective`'s return -- and narrow it instead at the
moment the display list is handed over.

That seam is reachable without touching any submodule. The port supplies
`create_render_context`, and can return a wrapper around recompui's context that
narrows the pending matrices in `send_dl` before delegating. The game gets a
widened frustum for the whole of its own frame; RT64 gets exactly the matrix it
gets today.

The risk to check is whether the game reads its projection matrix again after
submitting the frame, which would see the narrowed value.

## The fix as built

`src/render_context.cpp` returns a thin wrapper around recompui's renderer
context that overrides exactly one method:

    void send_dl(const OSTask* task) override {
        rayman2::narrow_pending_projections(rayman2::rdram_base());
        inner_->send_dl(task);
    }

Around it:

  * the `guPerspective` entry hook widens the aspect to the display's, so the
    matrix the game builds -- and culls from -- describes the frame the player
    is actually looking at;
  * the return hook notes the matrix down rather than narrowing it, so it stays
    wide for the whole of the game's frame;
  * `narrow_pending_projections` multiplies `[0][0]` back at the handover, so
    RT64 parses exactly the matrix it would have without any of this, and its
    Expand widens that as before.

No submodule is touched. The wrapper forwards `get_setup_result` and
`get_chosen_api` explicitly, because the base class keeps those as protected
members read by default getters and a shell that did not forward them would
answer with its own uninitialised copies.

The intro renders full width, with geometry reaching both edges and no seam.

## What still wants a person

Two questions a glance answers and a screenshot does not, because the cinematic
camera moves continuously and frames captured seconds apart in different runs
cannot be compared by apparent size:

  * do the mountains now survive to the edge of the frame, instead of going at
    the old 4:3 mark;
  * is the field of view unchanged from before this change.

If the answer to the second is no, the narrowing is not landing before RT64
reads the matrix, and the next thing to check is whether the game rebuilds its
projection after submitting.

## Third attempt: the aspect argument compounds

The `send_dl` wrapper was built and is correct in itself -- the matrix does stay
wide through the game's frame and is narrowed at the handover -- but the culling
boundary did not move, and the probe shows why.

With the widening on, successive `guPerspective` calls arrive reading:

    aspect=1.3393
    aspect=1.9369
    aspect=2.5500

2.5500 is this code's own clamp ceiling. **The game does not pass a fresh aspect
each frame: it keeps the one it was handed.** So widening the argument widens
what comes back next time, and the value climbs until it hits the cap. An
enhancement that drifts every frame is worse than the defect it was chasing.

(1.9369 was previously read as evidence of a second, letterboxed cinematic
camera. It was not -- it was this feedback, plus a window whose aspect had been
changed by hand during testing. A wrong reading of a real measurement, and worth
recording as such.)

All three attempts are disabled; `RAYMAN2_WIDESCREEN=frustum` re-enables them.

## The constraint for the next attempt

Whatever the fix is, **it cannot write to anything the game reads back**. That
rules out the aspect argument, which is the only input to `guPerspective` that
governs horizontal extent.

What is still unexplained is why the second attempt failed. It widened the
aspect and restored the matrix, and the boundary did not move -- which was read
as "the game culls from the matrix". But feedback was present in that build too,
so the aspect was climbing there as well, and the negative result may have had
nothing to do with where the culling reads from. That inference should be
treated as unproven.

So the open question is the same one as at the start, and it now needs answering
directly rather than by elimination: **what does Rayman 2's visibility test
actually read?** The honest way at it is RT64's frame inspector on a paused
frame -- comparing the draw calls submitted at the 4:3 boundary against what is
beyond it -- rather than more experiments that change one number and infer from
the picture.

## Fourth attempt: absolute, not relative

The feedback was the whole problem, and it was mine rather than the game's. The
code multiplied whatever aspect arrived, and the game hands back the aspect it
was given, so each frame widened the last until it saturated at the clamp.

Writing an **absolute** target instead of a multiplier makes that harmless: the
game hands back 1.7778, the code decides the answer is 1.7778, and it is a fixed
point rather than a runaway. The value the matrix is measured against is the
first aspect the game ever asked for, captured once and never updated, so
nothing downstream can move the reference either.

Measured, stable across a whole cinematic:

    guPerspective aspect=1.3393     (before the window size is known)
    guPerspective aspect=1.9417     (and stays there -- no climb)

and at the handover, both cameras narrowed back to the values an unmodified call
would have produced:

    send_dl: pending=2
    narrow 0x800E5F38 [0][0] 0.56053 -> 1.06865 (k=1.9065)
    narrow 0x800E7320 [0][0] 0.76328 -> 1.45518 (k=1.9065)

1.06865 is cot(69.644/2)/1.3393 and 1.45518 is cot(54.118/2)/1.3393, which are
the two cameras' un-widened horizontal scales. So the game culls against the
frame the player sees, and RT64 parses the matrix it would have parsed anyway.

The intro now draws geometry to both edges of the frame with no seam where the
4:3 boundary used to be.

## Still wants a look

Numbers say the framing is unchanged and the culling boundary is gone. Gameplay
is where that gets confirmed -- the mountains in the opening, and then anywhere
scenery used to wink out at the sides.

## The fourth attempt worked, and the culling stayed

The absolute-target version is correct and stable. Both ends were verified: the
aspect the game builds with holds at the display's without drifting, and the
matrix is narrowed back to exactly the un-widened value at the handover. The
game's frustum really was widened for the whole of its own frame.

Scenery still winks out at the sides.

**That rules out `guPerspective` entirely.** If the visibility test read the
aspect argument, or the matrix built from it, a genuinely widened frustum would
have moved the boundary. It did not. Rayman 2 culls against something else, and
nothing done at this function can reach it.

Turned off. `RAYMAN2_WIDESCREEN=frustum` re-enables the widening,
`RAYMAN2_NARROW=0` leaves the wide matrix in place through to RT64, and
`RAYMAN2_DDPROBE=1` prints both ends. The measurements are worth more than the
code, and whatever finds the real culling will want them.

## What four failures actually bought

  * `guPerspective` is identified for certain, with its two call sites and the
    ABI of its stack arguments -- a fixed point in a game with no decomposition.
  * The aspect it is given is 1.3393, which is 300/224, the framebuffer.
  * Its far plane is 8192 against a near of 32, so distance is not what limits
    what this game draws (issue 002).
  * The game retains the aspect it is handed, so anything written there must be
    an absolute value rather than a multiple, or it compounds every frame.
  * RT64 does not cull display lists: `cullDl` is a no-op.
  * And the visibility test reads neither the aspect nor the projection matrix.

## What to do next, and what not to

Not another experiment that changes one number and infers from the picture. That
has now been wrong four times, and each time the inference drawn from the
failure was itself wrong -- a second cinematic camera that did not exist, a
conclusion that the game culls from the matrix, a conclusion that it does not.
Changing an input and reading a photograph is too weak an instrument for this.

The next step is to look directly, with RT64's frame inspector
(`RAYMAN2_DEVMODE=1`, then F1), paused on a frame with the seam visible, and
answer one question that no experiment here has answered:

> Is the geometry beyond the boundary **absent from the draw calls**, or is it
> **submitted and then not drawn**?

Absent means the game culled it and the hunt is in the game's own code, which is
a reverse-engineering job of some size. Submitted means the game is innocent and
something between the display list and the screen is dropping it, which is a
much smaller and quite different search -- and it would explain why four changes
to the game's frustum did nothing at all.
