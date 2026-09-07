# Phase 04 findings — boot bring-up

**Gate not met.** The gate is "the Ubisoft logo, then the attract sequence,
rendering recognisably". The game does not render anything yet.

**What did change:** the port no longer crashes. It runs continuously with a
window open, executing recompiled game code, across an indefinite run. Four
faults were diagnosed and fixed to get there, and the screen is black rather
than gone.

| | Before phase 04 | Now |
|---|---|---|
| Reaches `recomp_entrypoint` | yes (phase 03) | yes |
| Resolves the entry stub's indirect jump | **no** | yes |
| Survives libultra init | **no** — access violation | yes |
| Runs indefinitely | **no** | yes, window open, CPU busy |
| Renders | no | **no** |

## The method that worked

Every fault in this phase was diagnosed the same way, and it is fast enough to
be worth writing down as a procedure:

1. The crash reporter (phase 03) prints the faulting address and the RDRAM base.
2. Subtract. The difference is the *game* address, because `MEM_W` maps a game
   address to `rdram + (addr - 0x80000000)`.
3. Read off which hardware bank it belongs to. Every fault so far has been a
   direct register poke, and the bank names the subsystem:

   | Offset from RDRAM | Game address | Register |
   |---|---|---|
   | `0x24800018` | `0xA4800018` | SI_STATUS |
   | `0x3FC007FC` | `0xBFC007FC` | PIF RAM status |
   | `0x24500010` | `0xA4500010` | AI_DACRATE |

4. `llvm-symbolizer` on the stack names the recompiled function.
5. Read that function's assembly and identify which libultra routine it is.

`VirtualQuery` explains *why* these fault rather than silently reading nonsense:
librecomp maps 512 MB read-write and leaves the rest of the 4 GB reservation
`PAGE_NOACCESS`, so a hardware address -- which lands hundreds of megabytes past
the RDRAM window -- hits the guard. The guard is doing its job.

## The strategic point: name the public API, not the plumbing

The obvious response to "the game pokes SI_STATUS" is to emulate SI_STATUS. That
is the wrong move, and the symbol lists say so.

N64Recomp sorts libultra into two sets. `reimplemented_funcs` are provided by
librecomp; naming one hands that function to the runtime. `ignored_funcs` are
provided by *nobody* -- the recompiler drops them and the port must supply them.
The split runs almost exactly along the public/private line:

| | Set | Naming it means |
|---|---|---|
| `osInitialize`, `osAiSetFrequency`, `osAiSetNextBuffer`, `osPiStartDma`, `osViSwapBuffer`, `osContInit` | **reimplemented** | the runtime takes over |
| `__osSiRawReadIo`, `__osSiRawStartDma`, `__osPiRawStartDma`, `__osViSwapContext`, `__osAiDeviceBusy` | **ignored** | *this port* must emulate the hardware |

So naming a public entry point deletes the whole subsystem beneath it. Naming
`osInitialize` -- proven from its body: enable CU1, set the FPU control word,
handshake with PIF RAM, install the exception vector -- removed the PIF access,
the SI polling and the exception-handler installation in one step. Emulating
PIF RAM to satisfy the game's own copy would have been days of work to
reproduce something librecomp already does.

The rule this phase adopts: **when a fault lands on a hardware register, walk
*up* the call chain to the nearest public libultra function and name that.**

## Named this phase

Each from its own body, never from a guess:

- **`func_80000400` size `0x50` + `func_80000450`** — the entry stub is 0x50
  bytes and ends with `jr $t2` where t2 holds `0x80000450`; splat sized it at
  `0xC0` and swallowed the function starting there. Because the jump is
  indirect, splat saw no call to it. This is what "Failed to find function at
  0x80000450" was.

  Worth noting: this exact change was tried in phase 03 and appeared to make
  things worse. That measurement was taken *through* the entrypoint
  sign-extension bug, which crashed the boot DMA long before any of it ran, so
  it said nothing at all. Re-tested on a sound boot path, it was simply correct.
  A negative result is only as good as the thing it was measured through.

- **`__osSiDeviceBusy` (`0x800115A0`)** — reads SI_STATUS, masks bits 0–1
  (DMA_BUSY | IO_READ_BUSY), returns whether either is set. Six instructions,
  size `0x18`. Implemented in `src/port_runtime.cpp` as "never busy": there is
  no transfer to wait for, and returning otherwise spins the caller forever.

- **`osInitialize` (`0x80003EE0`)** — see above.

- **`osAiSetFrequency` (`0x80007850`)** — called from main with `a0 = 22050`;
  body converts the frequency to a DAC rate in floating point and writes
  AI_DACRATE and AI_BITRATE.

## What the counter found

`src/rt64_context.cpp` now counts VI frames, display lists and dummy workloads,
and announces the first display list. It answered the question on the first run,
and the answer was none of the three possibilities above:

```
[rayman2] frames  63 (+63/s)  display lists 0 (+0/s)  dummy 10 (+10/s)
[rayman2] entering recomp_entrypoint -- the game thread is running
[rayman2] recomp_entrypoint returned                       <-- !
[rayman2] frames 609 (+61/s)  display lists 0 (+0/s)  dummy 10 (+0/s)
```

**The entrypoint returned**, within a second, and the VI then ticked at 60/s over
an empty screen indefinitely. Nothing was stuck and nothing had failed.

That is *correct* libultra behaviour, and reading main confirms it: it calls
`osInitialize`, sets the audio rate, calls `osCreateThread` with entry
`0x800004C0`, calls `osStartThread`, and returns. The game is supposed to
continue on the created thread.

The created thread never ran. The game's own `osCreateThread` and
`osStartThread` were being recompiled: they build an `OSThread` and push it onto
the game's idea of a run queue, but **ultramodern owns the scheduler in this
port and knows nothing about that structure**. The thread was created, marked
runnable, and never scheduled. Nothing crashed; the game simply ran out of
things to do.

Naming both -- each proven from its body, and both reimplemented by librecomp --
started the thread. The stack now shows it plainly:

```
run_thread_function          <- ultramodern scheduling the game's thread
  func_800004C0              <- the entry main passed to osCreateThread
    func_80002690
      func_8000B190
        ...
```

## The pattern this exposed, which is the phase's real finding

With threading handed over, the next faults were all the same shape: a read a
few bytes into a null pointer, deep in the game's libultra.

| Function | Fault | Identified as | How |
|---|---|---|---|
| `func_8000D080` | reads null+4 | `osGetThreadPri` | null-defaults to `__osRunningThread`, returns priority at 0x4 |
| `func_8000D430` | reads null+4 | `osSetThreadPri` | same, two args, inside a critical section |
| `func_800040F0` | writes null+0x12 | `osRecvMesg` | blocks while `validCount == 0`, copies `msg[first]` out |
| `func_8000D2C0` | reads null+0 | `osSendMesg` | index `(first + validCount) % msgCount` -- appends |
| `func_8000D130` | — | `osJamMesg` | index `(first + msgCount - 1) % msgCount` -- prepends |
| `func_80003DC0` | — | `osCreateMesgQueue` | six-field init, sentinel into `mtqueue`/`fullqueue` |
| `func_80003EC0` | — | `osGetThreadId` | as `osGetThreadPri`, reading id at 0x14 |

Two of these are worth keeping in mind when reading future faults.

**`osSendMesg` and `osJamMesg` are nearly identical** and are told apart only by
the insert index: send needs `validCount` to append at the tail and loads it;
jam does not, and adds `first + msgCount` to prepend at the head. Everything
before that -- the blocking, the NOBLOCK return, the yield -- is the same code.

**A fault's offset does not match the offset in the instruction** for anything
narrower than a word. `osRecvMesg` faulted at null+0x12 while the instruction is
`sh $s3, 0x10($v0)`, because `MEM_H` XORs the address by 2 (and `MEM_B` by 3) for
big-endian byte ordering. Subtract that before looking for the field.

The cause is one thing, not three. `osGetThreadPri` is
`if (t == NULL) t = __osRunningThread; return t->priority;`. The game's
`__osRunningThread` global (`D_8001A340`) is never set any more, because
ultramodern is the scheduler. So the null default resolves to null, and the
field read faults.

**Every part of the game's libultra that reads scheduler state has to become the
runtime's.** `D_8001A340` is read in **30 places** in the boot segment, so this
is a body of work rather than a couple of names -- but it is mechanical, each
one is proven from its body, and the fault signature (a small offset from null,
on the game thread) identifies the class instantly.

## Where it stops now

**No access violations at all.** Every libultra layer the boot path touches is
now the runtime's, and the game runs cleanly through all of it. It ends instead
with librecomp reporting:

```
Initializing recomp heap at offset 0x01000000 with size 0x1F000000
[rayman2] entering recomp_entrypoint -- the game thread is running
[rayman2] frames 123 (+60/s)  display lists 0 (+0/s)
Encountered break at original vram 0x8008F86C
```

That is a MIPS `break`, not a crash -- `func_8008F86C` is a three-instruction
stub whose whole job is to trap, i.e. the game's assert/panic routine. The call
site is a plain range check:

```
lw    $v0, 0xC($s0)      ; a field of some structure
sltiu $v0, $v0, 0x100    ; must be under 0x100
bnez  $v0, .L80088DAC    ; in range -- carry on
jal   func_8008F86C      ; out of range -- assert
```

So the game got far enough to check its own invariant and found it violated.
This is a **game-level** failure now, not a runtime one, and that is a different
and better kind of problem: the port is no longer the thing that is broken.

Phase 00 noted the ROM keeps its `__FILE__` strings, and this is where that pays
off -- the assert stub is reached from a specific source file, and finding which
one narrows the search enormously. Whatever is at offset 0xC of that structure
is either uninitialised or was filled from data that never arrived.

## Subsystems handed to the runtime

The whole of this phase, in one table. Each name was proven from the function's
body, and each is in the recompiler's *reimplemented* set, so naming it moves
the work to librecomp rather than obliging this port to emulate hardware:

| Subsystem | Named | What it removed |
|---|---|---|
| Init | `osInitialize` | PIF RAM handshake, SI polling, exception-vector install |
| Audio | `osAiSetFrequency` | AI_DACRATE / AI_BITRATE pokes |
| Threads | `osCreateThread`, `osStartThread`, `osGetThreadPri`, `osSetThreadPri`, `osGetThreadId` | the game's run queue and `__osRunningThread` |
| Messages | `osCreateMesgQueue`, `osRecvMesg`, `osSendMesg`, `osJamMesg` | blocking, yielding and the enqueue path |
| Video | `osCreateViManager` | `__osViInit`, the VI thread, VI register writes |
| Cartridge | `osCreatePiManager` | `__osDevMgrMain`, `__osPiRawStartDma`, PI register writes |

The pattern held every time: **name the public entry point, and the private
plumbing beneath it stops being reached.** Not one hardware register needed
emulating in the end.

## The `__FILE__` partitioning, and what it was actually worth

`tools/partition_by_file.py` finds the source paths the compiler embedded in the
cartridge's asserts, resolves every address the code materialises (both splat's
`%hi/%lo` symbolic form and the literal constants it writes out), and attributes
each function to the file it was built from. `docs/MODULE-MAP.md` is the result.

**Phase 00 oversold this, and the correction matters.** It predicted the assert
anchors would "partition a large fraction" of the functions into modules. The
measured yield is **36 functions across 28 source files** -- roughly one percent
of the ~3,000 in the port. Asserts with `__FILE__` simply are not distributed
the way that prediction assumed; most functions contain none.

What it *is* worth is different, and in one respect better than expected.
Twelve of the 28 files are **libultra's own sources**, and a libultra filename
identifies a function outright:

| Function | File | Therefore |
|---|---|---|
| `func_80008650` | `sirawread.c` | `__osSiRawReadIo` |
| `func_800086E0` | `sirawwrite.c` | `__osSiRawWriteIo` |
| `func_800115C0` | `sirawdma.c` | `__osSiRawStartDma` |
| `func_8000B0F0` | `pirawread.c` | `__osPiRawReadIo` |
| `func_80013520` | `epirawread.c` | `__osEPiRawReadIo` |
| `func_80011380` | `sprawdma.c` | `__osSpRawStartDma` |

The first three had already been identified from their instruction bodies
earlier in this phase, and the compiler's own strings agree. That is genuine
corroboration from an independent source, which is worth more than either
method alone -- and it means the technique's real use here is confirming
libultra identities rather than mapping the game.

The game-side files it did find (`Actions/Brain.c`, `Actions/Dynam.c`,
`Culling.c`, `Inters.c`, `HieMtStk.c`, `Specif/U_vpt.c` and the rest) give one
or two functions each. Useful as anchors, not as a map.

## It did not answer the question that prompted it

The point of doing this now was to find which source file the failing assertion
lives in. It cannot, and the reason is worth recording so nobody retries it:

```
func_8008F86C:
    break  255
    jr     $ra
```

The assert routine is a bare trap. It takes no filename, no line number and no
message -- so there is no `__FILE__` reference at the call site to attribute,
and `func_80088B5C`, the function that trips it, references no source path at
all. This is a different assert mechanism from the ones that do embed paths.

Narrowing that failure needs a different approach: the field at offset `0xC` of
the structure is required to be under `0x100`, so the question is what fills
that structure, and that is a data-flow question to answer by tracing
`func_80088B5C`'s caller rather than by looking for names.

## Tracing the assert: what the structure is

The failing check was traced from the `break` outwards. The whole chain sits in
the main segment and is reached from three sites in early initialisation:

```
func_80028BE0 / func_80028CA8 / func_80028D9C   (three call sites)
  func_80088A80
    func_80088AA8
      func_80088B00      walks a linked list
        func_80088B5C    serialises one node   <- asserts here
          func_8008F86C  break 255
```

**`func_80088B00` is a filtered list walk.** It loads the head from `*(a0)`,
and for each node tests a caller-supplied mask against a 16-bit field, calling
the serialiser only on a match, then follows `next`:

```
s0 = *(a0)                    ; list head
loop:
  v0 = *(u16*)(s0 + 0x10)     ; type/flags
  if ((s1 & v0) == 0) skip    ; s1 is the mask passed in a2
  func_80088B5C(s0, &out)
skip:
  s0 = *(u32*)(s0 + 0x0)      ; next
```

So the node layout, as used here, is `0x0` next, `0xC` the asserted field,
`0x10` a combined type/flags halfword. `func_80088B5C` dispatches on the top six
bits of that halfword (`flags & 0xFC00`, compared against `0x1000` and `0x3000`),
so the field is a small type tag in the high bits with flag bits below.

**The assert guards a narrowing conversion.** Immediately after the check:

```
lw  $v0, 0xC($s0)      ; the field
jal func_80088E68      ; (buffer, 1, 1, stream)
sb  $v0, 0x28($sp)     ; written as ONE BYTE
```

The value has to be under `0x100` because it is about to be stored with `sb`.
So the field is an identifier or index that the format allows one byte for, and
the assertion is the game checking that assumption before truncating. This is a
serialiser: it walks an object list, selects by type, and emits a compact byte
stream.

## What that does and does not settle

It settles the mechanism completely, and it rules out the boring explanations --
this is not a null pointer, not an unmapped address, and not a hardware register.
The list is being walked and nodes are being visited; one of them carries a value
in `0xC` that does not fit a byte.

It does not settle **why**, and the honest position is that this cannot be
answered by more reading. Two possibilities remain, and they need different
fixes:

1. The list or its nodes contain data the port never correctly filled -- the
   likelier of the two, since a retail cartridge does not normally trip its own
   assertions during boot.
2. The walk itself is wrong -- a bad head pointer would produce a plausible-
   looking chain of nonsense with no null to stop it.

Distinguishing them needs the runtime value of that field and the node address
it came from, and neither is available today: librecomp's `do_break` receives
only the faulting vram (`recomp.cpp:467`), with no context and no RDRAM, so the
crash reporter that served so well for access violations cannot help here.

**Three ways to get it, in increasing cost:**

- **Patch the instruction.** `recomp/rayman2.us.toml` supports
  `[[patches.instruction]]`, so the assert's branch can be neutralised for one
  run. That does not explain anything, but it answers a genuinely useful
  question cheaply: whether this invariant is load-bearing or whether boot
  simply continues past it. One config line and one rebuild.
- **The patches layer.** `RECOMP_HOOK` on `func_80088B5C` can print the node
  address and the field. This is the designed mechanism and the right answer,
  and it needs the MIPS cross-toolchain (LLVM 18.1.8) that BUILDING.md already
  lists -- currently scheduled for phase 06.
- **Trace the three callers** at `func_80028BE0`, `func_80028CA8` and
  `func_80028D9C` to find what builds the list in the first place.

The first is worth doing before the second, because if boot continues past the
assert then the invariant is advisory and the search moves elsewhere entirely.

## The instruction-patch experiment, and its answer

The question was whether the assertion is load-bearing. `[[patches.instruction]]`
answered it for two rebuilds and no new tooling.

**Attempt one NOPed the `jal` at the failing call site, and the break fired
anyway.** `func_8008F86C` turns out to be a *shared* assert routine with **ten**
callers, so silencing one site just moves to the next. Useful in itself: the
game asserts in ten places during boot, not one.

**Attempt two NOPed the `break 255` instruction inside the routine**, which
disables all ten at once -- the function becomes `nop / jr $ra / nop`. That is
the patch worth recording, because it converts "does this one check matter" into
"do any of the game's assertions matter":

| | Before | Assertions disabled |
|---|---|---|
| Breaks reported | 1 | **0** |
| Process lifetime | ~2 s, then exits | **indefinite** (20 s+, still going) |
| VI frames | stops at ~123 | **climbs steadily at 60/s past 1,090** |
| Display lists | 0 | **0** |
| Dummy workloads | 10 | 10, and not increasing |

So the answer is a clear **no, and it does not help**. Execution continues
happily past every assertion the game makes -- nothing downstream depends on
that invariant enough to fail immediately -- but the game still never submits a
display list. Bypassing the assert bought a process that stays alive without
drawing.

That is worth knowing precisely because it is negative. **The assertion is a
symptom, not the blocker.** Whatever stops this game reaching its render path is
upstream of the assert and independent of it, so effort spent on that one check
would have been wasted. The list data really is wrong -- the game says so, and
it is right -- but fixing the check would not have produced a frame.

**The patch was removed rather than kept.** With the trap disabled the game runs
on through data it has itself declared invalid, and anything observed in that
state may be an artefact rather than a finding. The exact stanza is recorded in
`recomp/rayman2.us.toml` so the experiment is one paste away, but the default
build stays honest. A diagnostic that quietly becomes permanent is worse than no
diagnostic.

**Where this leaves the search.** Two independent problems are now visible where
there appeared to be one:

1. Something fills those list nodes with a value that will not fit a byte. The
   game detects this itself. Finding the cause wants a `RECOMP_HOOK` printing
   the node address and field -- the phase 06 MIPS toolchain.
2. Something stops the game reaching the render path *at all*, and it is not
   the assertion. This is the one that blocks the phase 04 gate, and the
   counters say the game thread is alive while producing no graphics tasks.

The second is the one to chase first, and it needs a different instrument again:
where the game thread actually is while the VI ticks. A periodic sample of the
game thread's call site would say whether it is looping, blocked on a queue that
never fills, or quietly finished.

## The thread sampler, and a correction it forced

`src/thread_sampler.cpp` samples every thread periodically and reports two
things: instruction pointers that landed in our own module ("executing"), and
addresses found on the stacks of threads parked in a system wait, where the Rip
is inside ntdll and only the return address further up is informative. It is
opt-in behind `RAYMAN2_SAMPLE`, because suspending every thread is intrusive.

It suspends, reads the context, copies a bounded stack window, and resumes,
touching no lock in between -- doing anything else while a thread is suspended
risks deadlocking on whatever lock that thread was holding.

**Run in the default build it says almost nothing**, and for an instructive
reason: with assertions live the game dies after about two seconds, so the
cumulative totals are swamped by the idle period afterwards. One sample in 560
landed in our code, everything else in `moodycamel::Semaphore::wait` -- which is
ultramodern's worker threads idling with the game thread already gone.

**Run with the assert bypass, the picture is completely different:**

```
224 rounds, 192 executing our code, 9737 parked in system waits
executing:
     116  rayman2-recomp.exe+0x214740   -> func_8008F86C
      43  rayman2-recomp.exe+0x1867b    -> func_800004C0  funcs_48.c:6182
      17  rayman2-recomp.exe+0x1868a    -> func_800004C0  funcs_48.c:6184
       4  ... +0x18670 / +0x18683 / +0x1868e -> func_800004C0
```

`func_8008F86C` is **the assert routine**. With its `break` NOPed it became
`nop / jr $ra / nop`, and the game is calling it constantly. The rest of the
samples cluster in six addresses spanning about thirty bytes inside
`func_800004C0`, the entry of the thread `main` created: a tight loop.

### The correction

The bypass experiment concluded that "execution continues past every assertion"
and that the assertion was therefore "a symptom, not the blocker". **That
reading was too generous, and the sampler disproves it.** The game does not
proceed past the assert and get stuck somewhere else. It enters a loop that
re-runs the failing operation and re-asserts, indefinitely.

The distinction matters because it changes what to work on. "Not the blocker"
pointed the search away from the list data; "loops forever re-asserting" points
it straight back. The bad value in that node really is what stops this game
booting, and the earlier conclusion would have sent the next session hunting a
second, non-existent problem.

Worth noting how the mistake was possible: with assertions disabled the process
stays alive and the VI keeps ticking at 60 Hz, which *looks* exactly like a game
running normally without drawing. Uptime and frame counters cannot tell a
running game from a spinning one. Only sampling where the code actually is could.

### What this settles

- The game thread is **spinning**, not blocked and not finished.
- It spins in `func_800004C0`, the thread `main` created, re-attempting the
  operation that fails its own invariant.
- The invariant failure is on the critical path to booting.
- So the right next investment is the one deferred earlier: a `RECOMP_HOOK` on
  `func_80088B5C` printing the node address and the field at `0xC`, to find what
  fills it. That needs the phase 06 MIPS toolchain (LLVM 18.1.8), and it is now
  clearly worth setting up rather than a maybe.

## The hook, and a correction to the whole assert trace

### The MIPS toolchain was not needed

It is available -- WSL carries **LLVM 18.1.8** with the `mips` big-endian target
and `ld.lld`, exactly the pin BUILDING.md asks for, and the Windows LLVM (22.1.8)
is far too new. But none of it is required for this, because N64Recomp has a
much lighter mechanism than the `patches/` layer:

```toml
[[patches.hook]]
func = "func_800004C0"
before_vram = 0x800006AC
text = "rayman2_debug_site(rdram, 0x800006ACu);"
```

`FunctionTextHook` splices that text verbatim into the recompiled C, where
`rdram` and `ctx` are both in scope. No cross-compilation, no separate ELF, no
reference symbol tables. The helpers live in `src/debug_node.cpp` and are
declared in `include/port_runtime.h`, which is force-included into every
generated source -- which is what makes injected text compile.

### Three false starts, and what each taught

1. **Hooked the call site the disassembly trace had identified. It never fired.**
2. **Hooked the assert routine's own entry.** It fired, reporting `$ra == 0`.
3. **Swept the call sites** -- and found ten, none of which fired, because the
   sweep had scanned only `asm/main.s`. There are **22** across boot, main and
   aux.

Hooking all 22 answered it immediately.

### The answer, and the correction

The assert fires at **`0x800006AC`**, in the **boot** segment, inside
`func_800004C0` -- the thread `main` creates and starts. That is precisely where
the thread sampler had already located the spin, so the two instruments agree.

```
jal   osCreateThread
jal   osStartThread          ; start the child thread
addiu $s0, $zero, 0x1        ; s0 = 1
.L8000069C:
lhu   $v0, D_800250D8        ; load a 16-bit flag
.L800006A4:
bne   $v0, $s0, .L800006A4   ; spin while flag != 1 -- v0 is NOT reloaded
jal   func_8008F86C          ; flag == 1 -> trap
j     .L8000069C             ; reload and go round again
```

**So the entire list-serialiser trace was the wrong path.** The sections above
that follow `func_80088B5C`, its node layout, the field at `0xC` and the `sb`
that narrows it are all accurate as *description*, and all irrelevant as
*diagnosis*: that assert never fires. It was found by searching for a call to
the assert routine and finding one, not by establishing which call actually
happens. The evidence was already there and was under-weighted -- NOPing that
site's `jal` did not stop the break, which by itself proved the firing site was
elsewhere.

### What the real site says

`main` creates a thread, starts it, and then loops on a 16-bit flag at
`D_800250D8`: it spins while the flag is not 1, and traps when it is. The flag
lives in the boot segment's bss. The trap is reached, so the flag reads 1 when
the code expects to wait on it -- either it is genuinely set, or the memory
backing it is not what the game expects.

This is a far better place to be than the serialiser. It is four instructions,
in the boot thread, on one variable, immediately after the thread hand-off that
phase 04 already had to fix once.

### The lesson, again

Every instrument in this phase has overturned a conclusion reached by reading:
the counters overturned "stuck in a wait", the sampler overturned "not the
blocker", and the hook overturned the whole assert trace. The pattern is
consistent enough to state as a rule: **in this codebase, a plausible path found
by reading is not evidence that it is the path taken.** Ask the running program.

## Root cause: osStartThread does not write OSThread.state

Following the real assert site to its end found the reason the game does not
boot, and it is not in the game.

### What the boot thread is doing

`func_800004C0` creates the game thread, starts it, and then:

```
addiu $s0, $zero, 0x1        ; s0 = 1
.L8000069C:
lhu   $v0, D_800250D8        ; a halfword in boot bss
.L800006A4:
bne   $v0, $s0, .L800006A4   ; spin while it is not 1
jal   func_8008F86C          ; it IS 1 -> trap
```

`D_800250D8` is not a standalone flag. Dumping the memory around it at runtime
shows a structure starting at `0x800250C8`:

| Offset | Value | Field |
|---|---|---|
| `0x00` | `00000000` | `next` |
| `0x04` | `00000008` | `priority` = 8 |
| `0x08` | `8001D138` | `queue` |
| `0x10` | `0001` | **`state`** |
| `0x14` | `00000003` | `id` = 3 |

That is an **`OSThread`**, and the layout matches the one established earlier
from `osStartThread` and `osGetThreadPri`. So the boot thread is polling the
child thread's `state`, waiting for it to read `OS_STATE_STOPPED` (1), and
trapping when it does. On hardware that poll never succeeds: the thread is
running, so the boot thread idles there forever, which is exactly what an idle
thread should do. The trap is the "the game thread died" path.

### Why it reads STOPPED

From `ultramodern/src/threads.cpp`:

```cpp
extern "C" void osStartThread(RDRAM_ARG PTR(OSThread) t_) {
    if (thread_self) {
        ultramodern::schedule_running_thread(PASS_RDRAM t_);   // does not touch t->state
        ultramodern::check_running_queue(PASS_RDRAM1);
    }
    else {
        t->state = OSThreadState::QUEUED;                       // only this path writes it
        resume_thread(t);
    }
}
```

Called from a game thread -- which is this case -- it schedules the thread and
**never writes `state` back into RDRAM**. The field keeps whatever
`osCreateThread` left, which is `STOPPED`. The runtime's own scheduling is
correct and the thread really does run; it is only the game-visible copy of the
state that is stale.

So the boot thread's poll succeeds on its first read and the game traps. Nothing
is wrong with the game, the recompilation, or the data.

### The shape of this bug, which has now appeared three times

This is the same failure as `__osRunningThread` reading null and as the game's
own `osCreateThread` never scheduling anything: **the game reads libultra state
that ultramodern owns and does not mirror**. The first two were fixed by naming
functions so the runtime provides them. This one cannot be, because the function
*is* already the runtime's -- it simply does not maintain a field the game
inspects directly.

That makes it the first problem in this port that has to be fixed in the runtime
rather than in configuration.

### Fixing it

The repository already consumes a fork of N64ModernRuntime (the `controller-pak`
branch), so there is somewhere to put this. The change is small: on the
`thread_self` path, set the game-visible `state` to match what the scheduler has
actually done, as the other path already does.

Worth confirming before writing it: which of `QUEUED` / `RUNNING` the game
expects to see between `osStartThread` and the thread being scheduled, since the
poll only cares that it is not `STOPPED`. Anything other than 1 unblocks this
particular case, but the field is observable to any game, so it should be right
rather than merely non-1.

## Still outstanding

- **Nineteen functions in the boot segment poke hardware registers** (a scan for
  `lui` of the register banks finds them). Four are now named or accounted for.
  The rest are ahead, and most should disappear as their public callers get
  named rather than needing individual attention.
- **The audio microcode** is still not recompiled, so `src/rsp.cpp` reports
  audio tasks complete without synthesising anything. That is phase 05.
- **`__osGetSR` returning 0** remains an unexamined approximation now that real
  game code is executing.
