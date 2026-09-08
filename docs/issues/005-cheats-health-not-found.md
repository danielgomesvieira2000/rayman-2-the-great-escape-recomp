# 005 — cheats: the value found was the health bar, not the health

**Status:** parked. The Cheats tab and the Infinite Health cheat are removed.
The tooling that found things is kept, and so is what it found, because the next
attempt should not have to rediscover any of it.

## What was attempted

A Cheats tab in the frontend, starting with Infinite Health, implemented as a
repeated write into the game's memory rather than a patch to its code.

## What went wrong, in order

Three mistakes, each of which was found only after the one before it was fixed,
and the last of which invalidates the other two.

1. **The address lived in an environment variable.** The toggle in the tab did
   nothing when the game was launched normally, and correctly reported "the
   address is not known", which is true and useless to somebody who has just
   turned a cheat on and died.
2. **The write width defaulted to one byte** while the value is a four-byte
   float, so it read `0xCC` where `0x42480000` belonged -- a single byte out of
   the middle of a float, useless to read and destructive to write.
3. **The address was a heap address, hardcoded.** `0x8023xxxx` is where Rayman's
   object happened to land in the attract demo. In a real session the same
   address read `0x00000000` and `0xCCCCCCCC` -- memory the game had never
   written. Locating it at run time fixed that much.

And then the one that ended it:

4. **The value found was not health.** With it held, the health bar stopped
   moving and the player went on taking damage and dying from hits. So the
   search had found the value that *drives the bar*, and the real health is
   somewhere else entirely.

## Why the measurements did not catch it

They could not have. Every confirmation was run against the attract demo, and
the attract demo was also the only evidence that the address was right -- so the
177 samples that "confirmed" the cheat were a tautology: the address was valid
there because it had been found there.

The single observation that settled it came from a person playing: *the bar is
full and I am still losing health*. Nothing available from inside the port
distinguishes "health" from "the number the health bar is drawn from", because
both fall when Rayman is hit and both are floats and both track each other
exactly.

## What was found, and what it actually is

An F5/F6/F7 search during play, narrowing 2,097,152 words to four:

    0x800EF6D4   float, static data
    0x801EFF4C   integer, heap             (never explained)
    0x80231870   float, heap
    0x802318E0   float, heap

Watched through a demo with nothing held, the three floats step together:

    0x800EF6D4   0x80231870   0x802318E0
        50.000       15.000       50.000
        43.333       13.000       43.333
        36.667       11.000       36.667
         ...            ...          ...
         0.000        0.000        0.000   <- death
        50.000       15.000       50.000   <- respawn

`0x80231870` steps in whole numbers, 15 down to 1, two per hit, and the other
two are exactly ten thirds of it at every sample. That reads like health and a
display value derived from it. **It is not.** Holding `0x80231870` freezes the
bar and does not stop damage, so all three belong to the DISPLAY chain, and the
whole set should be treated as "what the HUD draws".

That is the useful finding: these four are ruled out, and they are ruled out for
a reason that a search cannot see.

## Where a next attempt should look

* **Not with a damage-driven search.** Every value that falls when Rayman is hit
  falls when the bar does, so the same four come back. The search needs a
  discriminator that separates health from its display -- for instance freezing
  the display chain first, so that anything still falling is not the display.
* **The instant-death paths do not go through health at all.** Falls, drowning
  and instant hazards kill regardless, so even a correct Infinite Health is not
  invincibility, and "I still died" would not have settled anything on its own.
* **A code-side hook may be the honest answer.** A repeated write from the event
  pump can only hold a value the game is not recomputing faster. The damage
  routine itself is what a real infinite-health cheat would patch, and finding
  it is a disassembly problem rather than a memory-search one.

## What is kept

* `src/memory_search.cpp` -- the F5/F6/F7 search, and `RAYMAN2_FREEZE`. Both are
  debugging tools that promise nothing.
* `RAYMAN2_WATCH` decodes each word as hex, signed integer and float, which is
  what made the table above readable at all.
* Nothing user-facing. A Cheats tab whose one cheat freezes the HUD while the
  player dies is worse than no tab.
