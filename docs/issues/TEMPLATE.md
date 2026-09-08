# NNN — one line, what looks wrong

Copy this to `docs/issues/NNN-short-name.md`, put the screenshots next to it, and
fill in what you can. Anything you leave blank is fine; anything you fill in is
search space someone does not have to cover.

Filing it here rather than in a chat message is deliberate. It survives the
session, the image and the notes can be read together, and when it is fixed the
file becomes the regression record — which is what stops it coming back.

**Status:** open | fixed in `<sha>` | not a bug (this is how the game looked)

---

## What is wrong

What you see, and what you expected instead.

If a real N64 or an emulator looks different at this spot, say so. "Is this a
port bug or is this how the game looked" is a real question — Rayman 2 on N64
was rough in places — and one comparison screenshot settles it far more cheaply
than reasoning about the renderer.

## Where

The level, and how to reach this exact spot.

A Controller Pak image parked just before it is worth more than any description:
copy `controller_pak_1.pak` out of `%APPDATA%\rayman2-recomp` and drop it beside
this file. Reaching the bug in seconds instead of replaying half an hour is what
decides whether it gets fixed twice or once.

## Is it stable?

- Every frame, or intermittent?
- Only while the camera moves, or also standing still?
- Only at this spot, or everywhere of this kind of geometry?

## The three-toggle triage

Thirty seconds in the Graphics tab, one setting at a time, and it eliminates
most of the search space before anyone reads a line of code.

| Change | Result | What it means if it changes the defect |
| --- | --- | --- |
| Internal resolution | | scales with it → a renderer or upscaling issue, not the display list |
| Aspect ratio, Expand ↔ Original | | only when widened → culling, or 2D anchoring |
| Antialiasing off | | gone → a coverage or edge issue |

## The frame itself

Turn on **Developer Mode** in the Graphics tab and press **F1** for RT64's frame
inspector. Pause on the bad frame and walk framebuffer pairs → projections →
draw calls; highlighting a call shows which geometry it is.

You do not have to understand what you are looking at. A screenshot of that
panel, or one line — "draw call 37 in projection 1 is the water" — turns a day
of guessing into an hour.

## Evidence

- `screen.bmp` — what was on screen
- `vi-framebuffer.bin` — what the game drew, before any scaling or widening.
  A defect visible here came from the game or the display list; one visible only
  in the screenshot came from the renderer.
- the session report `.txt` — the event log covering the minutes before this
- the settings block from the capture's `ISSUE.md`

Press **F9** while the defect is on screen and all of that is written for you,
into `debug-report/captures/`, together with a filled-in copy of this template.

## Investigation

Filled in by whoever works it. What was ruled out matters as much as what was
found — a negative result recorded here is one nobody has to reproduce.

## Fix

What changed and why, and how it was verified. Ideally: the save that reproduced
it, and a note that the same save now looks right.
