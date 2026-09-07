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

## Root cause: OSThreadState used the wrong numeric values

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

### The actual cause was narrower, and worse

Checking before writing the fix was worth it, because the first diagnosis --
"osStartThread does not write state" -- was only half right. It does not write
it on that path, but the field was not left unwritten either. The scheduler sets
it, and the value it sets is wrong:

```c
typedef enum { STOPPED, QUEUED, RUNNING, BLOCKED } OSThreadState;   // 0,1,2,3
```

ultramodern enumerated these from zero. libultra defines them as **bit flags** --
`OS_STATE_STOPPED 1`, `RUNNABLE 2`, `RUNNING 4`, `WAITING 8` -- and the game
proves it uses those numbers: its own `osStartThread` compares `state` against 1
and 8 and writes 2, and its `osRecvMesg` writes 8.

So ultramodern's `QUEUED` is **1**, and libultra's `OS_STATE_STOPPED` is also 1.
The scheduler queues the thread, writes 1, and the idle thread reads "stopped".
Not a missing write: a value collision, in a field that is part of the ABI
because `OSThread` lives in RDRAM and games read it directly.

### The fix, and why it is safe

`OSThreadState` now carries libultra's values, in the fork's `controller-pak`
branch (`ultramodern/include/ultramodern/ultra64.h`). The change is confined to
that boundary: every use of the field inside ultramodern is symbolic
(`OSThreadState::STOPPED`, `::QUEUED` -- six sites in `threads.cpp` and
`scheduling.cpp`), and the only numeric comparisons on a member called `state`
elsewhere in the tree belong to the unrelated VI state flags.

**Verified, not assumed.** With it applied the game runs indefinitely with no
break and no trap, where before it died within two seconds on every run. The
sampler then showed the boot thread spinning inside the idle poll -- which is
exactly what an idle thread should do, and what the hardware does.

An aside worth keeping: that sampler run was dominated by
`rayman2_debug_flag`, the diagnostic hook still injected into the loop. A hook
placed inside a spin runs at spin frequency. It has been removed, along with the
rest of the hooks; `recomp/rayman2.us.toml` documents how to put one back.

## Still outstanding

- **Nineteen functions in the boot segment poke hardware registers** (a scan for
  `lui` of the register banks finds them). Four are now named or accounted for.
  The rest are ahead, and most should disappear as their public callers get
  named rather than needing individual attention.
- **The audio microcode** is still not recompiled, so `src/rsp.cpp` reports
  audio tasks complete without synthesising anything. That is phase 05.
- **`__osGetSR` returning 0** remains an unexamined approximation now that real
  game code is executing.

## Why no display lists were submitted

The question turned out to have not one answer but a series of them, all the
same shape. The recompiler replaces libultra function by function, matching on
symbol *name*; every libultra routine this port had not yet named stayed as
recompiled game code, and recompiled game code that writes a hardware register
writes to an address the runtime does not model. Each such routine is a seam,
and each seam had to be found by asking the running program rather than by
reading.

The chain, in the order it was found:

1. **osPiStartDma (0x8000BB80).** The boot thread loaded the main and aux
   segments through a wrapper that starts a PI DMA and blocks in `osRecvMesg`.
   As game code that routine posted an OSIoMesg to the PI manager's command
   queue -- but `osCreatePiManager` is librecomp's, so the game's manager thread
   did not exist and nothing ever read the queue. The boot thread waited
   forever, the main segment was never in RAM, and nothing could be drawn.

2. **osContInit, osContStartQuery, osContStartReadData, osContGetReadData,
   osContGetQuery.** The aux segment's init talks to the PIF. As game code that
   meant writing 0xA4800000, which resolves 0x24800000 bytes past the RDRAM base
   -- inside the guard region, which is why it faults rather than corrupting
   silently. The reader halves had to be replaced alongside the starters:
   librecomp does not fill the game's `__osContPifRam`, so a game-code reader
   unpacks a buffer nobody writes, which looks like a controller that is
   permanently idle -- worse than a crash.

3. **osPfsInitPak (0x8000A320).** Same fault, same cause, on the Controller Pak
   probe.

4. **Static section registration.** librecomp fills its address-to-function map
   only from `load_overlays(0x1000, entrypoint, 1 MB)`, deriving each section's
   RAM address from its ROM offset. That is right for boot and wrong for the
   other two, which the game DMAs itself to fixed addresses. Direct calls
   between recompiled functions are ordinary C calls and never consult the map,
   so the whole main segment ran correctly until the first *indirect* call into
   it. Fixed in `src/register_sections.cpp`, called from the port's one
   remaining hook.

5. **osSetIntMask (0x8000CE00).** The first seam reached from real game code,
   ten frames deep in the main segment.

6. **osSpTaskLoad, osSpTaskStartGo, osViSetMode, osViSetSpecialFeatures,
   osViSetYScale, osViSwapBuffer, osViBlack.** The submission and video paths.
   As game code, `osViSwapBuffer` set a bit in a structure nothing read.

7. **osViSetEvent (0x80008040) and osSetEventMesg (0x80004220).** The
   registrations. A frame loop is paced by retrace and the controller code
   blocks on SI completion; both ask to be told through a queue, and as game
   code both only wrote a pointer into a table in RDRAM. ultramodern keeps its
   own event table and posted to what it had been told about, which was nothing.
   `osSetEventMesg` is the general case and supersedes the narrower fix made in
   the runtime fork's `osContInit`.

8. **osAiGetLength, osAiSetNextBuffer.** Reached only once the previous fix let
   a second game thread start.

Two changes were made in the runtime fork (`lib/N64ModernRuntime`, branch
`controller-pak`): `osContInit` now registers the queue it is passed for
OS_EVENT_SI, and `send_si_message` refuses to enqueue when no queue is
registered rather than handing `do_send` a null pointer to fault on.

One port-side inconsistency was corrected: `get_connected_device_info` had been
advertising a Controller Pak in port 1 while every librecomp Pfs entry point
answers `PFS_ERR_NOPACK`. The game was being told a pak was present and then
denied it. It now reports no pak, which is a state the shipped game had to
handle; the two must change together if pak support is implemented.

### Where it stands

The game now boots, loads its segments, runs its own `main`, reaches its main
loop, starts a second thread, and submits an RSP audio task -- which the port
answers with the deliberate stub from `src/rsp.cpp`, since the audio microcode
is phase 05. Nothing crashes and nothing hangs in the runtime.

It still submits no *graphics* task. The main thread is inside its state-entry
chain, in a wait loop at func_800A1190 that spins until func_800A3878 returns
0x7FFE. Read at runtime, the code it actually gets is 0x1004, with
`D_800C9520` = 1 and `D_800C952C` = 1 -- so the subsystem is initialised and
the channel is enabled, and the check that fails is the last one: the
descriptor at `D_800CF624[0]` carries a capability count in its halfword at
offset 6 that is too small for the requested bit. That descriptor is allocated
and filled in func_800A2AD0 by way of the func_8008xxxx audio library, which is
where the next round should start.

### Method

Two instruments did nearly all the work, and both are in the tree:

- `rayman2_debug_count` in `src/debug_node.cpp`, driven by `[[patches.hook]]`
  probes on call sites. Bisecting a call chain one function at a time, with one
  probe per call, located each blocker in a few minutes without any guessing
  about which callee mattered.
- The crash reporter's stack walk plus `llvm-symbolizer` against the
  RelWithDebInfo build, which turns a faulting address into the full recompiled
  call chain and named the caller outright several times.

Two lessons are worth carrying forward. First, `$ra` is useless for identifying
callers here: N64Recomp emits `jal` as a plain C call and never assigns
`ctx->r31`, so it reads 0 in every recompiled function -- the native call stack
is the caller chain. Second, an instrument that only reports periodically
cannot distinguish "stopped" from "slow", and both readings send you somewhere
different; the raw per-call trace with timestamps settled in one run what two
rounds of summaries had left ambiguous.

## The wait was the game's own save prompt

The previous section ended by naming a wait loop at func_800A1190 and reporting
that func_800A3878 was failing with 0x1004. **That reading was wrong**, and the
way it was wrong is worth recording: 0x1004 was read out of `D_800CF620`, which
is a shared last-error global, not this call's result. It had been left there by
an unrelated caller. Reading a global that any code may write and attributing it
to the call in front of you is the same mistake as trusting `$ra` -- plausible,
cheap, and not evidence. Hooking the function's own return register settled it
in one run.

What the routine is actually doing became obvious once the game was asked
instead of read. func_80090BB8 formats the game's own messages, and a hook on
the point where it receives the text pointer prints them:

    "=and press the #5/5/31#$# Button."
    "=to continue without saving."

This is the Controller Pak save prompt. The game is not hung in the runtime; it
has stopped to ask the player a question, and it has been sitting there for
every run since the event queues were fixed.

### What that ruled out, and what it ruled in

The whole libultra input path is exonerated, by measurement rather than by
argument. With a key held, the OSContPad that librecomp writes at `D_800CF590`
reads 0x9000, the game's own copy at `D_800CF370` reads 0x9000, and the poll
chain -- func_80082E5C -> func_800A24B0 -> func_800A3BB0 -> func_8009FF70 ->
func_8009F38C, with the controller thread func_8009F2A0 on the other end --
turns 2.4 million times in fourteen seconds. Buttons reach RDRAM correctly.

Two real defects are left, and both are in the game's own input-binding layer:

1. **func_800A3878 tests a rising edge, not a level.** It requires the bit to be
   present in the current sample and absent from the previous entry of a
   sixteen-byte ring at `D_800CF6A4[port]`. A permanently held button therefore
   produces exactly one edge, at whatever moment sampling begins, and none
   afterwards. The first version of the RAYMAN2_AUTOPRESS diagnostic held the
   buttons down and so looked identical to no input at all; it now pulses at
   4 Hz, which is the correct shape for this test.

2. **The descriptor at `D_800CF664[0]` is not initialised.** It is 0x1C bytes,
   allocated by func_800A4174 in func_800A2AD0 and stored straight into the
   table without being cleared. Its supported-button mask at offset 4 reads 0,
   so `(button & mask) == button` can never hold; and its sample counter at
   offset 0xE starts from roughly 0xCCCC rather than 0 -- it increments
   correctly (0xCCCD, 0xCCCE, 0xCCCF ...), so the sampler is running, it is
   simply counting up from garbage. RDRAM itself is fine: librecomp allocates it
   with VirtualAlloc(MEM_COMMIT), which is zero-filled, so this fill is the
   game's own and the mask is being left unwritten rather than clobbered.

An experiment confirmed the diagnosis without leaving anything behind. Patching
the single instruction at 0x800A3920 (`and $v0, $a1, $v0` -> `or $v0, $a1,
$zero`) makes the mask test trivially true; with that plus a pulsing press,
func_800A3878 returns 0x7FFE and the wait at func_800A1190 completes -- the
game gets past its own save prompt. The patch has been removed. Leaving a
falsified check in place would make every later observation suspect, for the
same reason the assert-trap patch was never left in.

So the remaining question is narrow and well posed: what should be writing the
supported-button mask into `D_800CF664[i]+4`, and why has it not run? The
descriptor is created in func_800A2AD0 alongside its siblings in `D_800CF624`
and `D_800CF6A4`, next to calls into the func_8008xxxx library, which is where
to look next.

The port still submits no graphics task. Nothing crashes, nothing hangs in the
runtime, and the game is now stopped on a screen it means to be stopped on.

## The scheduler has no preemption, and the game assumes it does

### First, a correction

The previous section concluded that the descriptor at `D_800CF664[0]` was
uninitialised, on the evidence that its supported-button mask at offset 4 read
zero. **That was a measurement artefact.** func_800A3BB0 writes that field with
`sw`, so it is a *word* holding a zero-extended halfword -- 0x00009000 -- and
the probe read it with `MEM_HU`, which returns the upper half of a big-endian
word and is therefore always zero. Read as a word it holds 0x9000, exactly the
buttons being pressed.

With that corrected, and the auto-press pulsing rather than holding,
func_800A3878 returns 0x7FFE and the wait at func_800A1190 completes **with no
instruction patch at all**. The game dismisses its own Controller Pak prompt on
a button press, as it should. The binding layer was never broken; two successive
readings of it were.

The lesson is narrow and worth keeping: when a probe reports zero, check the
width of the field before concluding anything about the program. A halfword read
of a word field is not a null result, it is the wrong question.

### The actual defect

Past the prompt, func_800FA95C runs to its last call, func_8008F344, which is
the tail of the game's graphics initialisation:

    func_8008EF6C();
    while (func_8008EF6C() != 0) { }      // reads one byte, calls nothing
    func_8008F314();

The byte is cleared by a worker thread at func_8008EF78, which sits in
`osRecvMesg` on queue `D_800CF0C0`. Measured over fourteen seconds, the spin
turned **519 million times** while the worker ran **21 times** and then stopped
for good.

That is a deadlock, and it is structural rather than particular to this game.
libultra's scheduler is preemptive: a counter interrupt fires no matter what the
running thread is doing, so code is entitled to spin on a flag and expect
somebody else to clear it. ultramodern's is not. `check_running_queue` is called
from exactly three places -- `osSendMesg`, `osRecvMesg` and `osJamMesg` -- and
`dequeue_external_messages`, which is the only thing that delivers VI retrace,
SP and DP completion, PI and SI, is called from the same three. A game thread
that spins without calling any of them stops the entire game: no other thread
runs and no external event is ever delivered.

The fix is `src/spin_yield.cpp`, injected into the loop body by a hook. It does
what the counter interrupt would have done -- deliver a pending external message
and hand the CPU to a higher-priority runnable thread. The wait is bounded to a
millisecond rather than indefinite: VI retrace alone would supply a message
every frame, but a spin that turns into a hard block if that supply ever stops
trades a visible busy-wait for an invisible hang, and the bound costs nothing.

With it, the worker runs continuously instead of stopping at 21, the spin exits,
func_800FA95C returns, and func_800292F4 gets past the initialisation call it
had been stuck on since this phase began.

This is the first defect found in this port that is a property of the runtime's
execution model rather than of a symbol left unnamed, and it will not be the
last: any other busy-wait in this game will fail the same way and need the same
treatment. A general fix -- pumping external messages from outside the game
threads -- would be better than a hook per loop, and is worth considering if a
third one turns up.

The port still submits no display list. Nothing crashes, nothing hangs, and the
game is now further into its own initialisation than at any previous point.

## The main loop runs, and the graphics pipeline is mapped end to end

Fixing the busy-wait deadlock unblocked everything behind it. The twenty-call
initialisation chain in func_800292F4 now completes, func_800263C8 runs through
its state setup, and **the game's main loop turns continuously** -- roughly
forty-eight game frames a second, with the per-frame body's four indirect calls
all firing every frame. A handful of runs produced the port's first display
lists. It is not yet steady, and the reason is now known precisely.

### A tool for the deadlock class

`tools/find_spin_loops.py` looks for the defect the previous section described,
so the next one is a lookup rather than an investigation. The obvious rule --
"a loop with no `jal`" -- is wrong, and wrong in the way that matters: the known
case does contain a call, to a three-instruction leaf that reads one byte. What
makes a loop dangerous is whether anything it calls can *reach* a yield point.
So the tool builds the call graph, marks every function that can reach
osSendMesg, osRecvMesg or osJamMesg at any depth, propagates each function's
global reads and writes through non-yielding calls, and reports loops that read
a global they never write and can never yield. Results are ranked by loop
length, because a wait on another thread is almost always a handful of
instructions.

It ranks func_8008F344 first, which is the loop that was found the hard way, and
second func_800FADC8 -- four instructions, spinning on the same byte through the
same leaf. That one had not been reached yet and is patched now, because finding
the identical defect a second time by bisection would be a waste.

### Where the display list actually stops

The pipeline is fully mapped, and every stage of it runs:

    frame body                    -> func_80027B98        every frame
      -> func_8008E5B8            -> func_8008EDC0        reached
         func_8008EDC0 terminates the list (G_RDPFULLSYNC, G_ENDDL) and posts
         to the task queue D_800CE2BC
    task thread func_8008F5C0     receives command 0x378  516 times
      -> state machine on D_800C8D24, states 3 and 5
      -> gate func_800AE9DC       returns 1 about half the time
      -> func_8008F378            entered 524 times
         -> osRecvMesg(D_800CE2BC, NOBLOCK)               returns -1, always
         -> osSpTaskLoad                                  never reached

Every RSP task the port has submitted is type 2, audio -- 449 of them in
fourteen seconds, zero graphics. Reading the type took two attempts: the first
hooked before the `jal osSpTaskLoad` and read $a0, which is set in the *delay
slot*, so it reported whatever the register happened to hold from an earlier
call. That is the same class of mistake as the halfword read of a word field
recorded above, and the same remedy applies -- know what you are reading before
you believe it.

So the remaining question is narrow: func_8008EDC0 posts the "a list is ready"
message, and func_8008F378's non-blocking receive of it never finds one.
func_8008EDC0 begins with a *blocking* osRecvMesg on D_800EFE68, so the next
thing to establish is who posts to that queue and whether the producer is
getting past it every frame or only once.

Named this round: **osViGetCurrentFramebuffer** (0x80007CD0), which reads
framep out of __osViCurr. func_8008F378 uses it to skip a frame whose buffer the
VI is already showing; as game code it read a structure the runtime no longer
maintains, since osViSwapBuffer is librecomp's and updates ultramodern's state
instead. Naming it did not by itself start the display lists flowing -- the
bail-out above happens earlier -- but it is correct regardless and would have
become the next blocker.

## The game renders

Two defects stood between the frame loop and the screen. Both are the *same*
defect as the busy-wait deadlock, in forms that did not look like it.

### The message-queue control is never installed

ultramodern delivers everything that happens outside the game -- VI retrace, RSP
and RDP completion, PI and SI -- through a queue drained inside osSendMesg,
osRecvMesg and osJamMesg. When a delivery finds the destination queue
momentarily full it either puts the message back or discards it, and which of
those it does comes from a bitset installed by
`ultramodern::set_message_queue_control`.

Nothing calls it. Not the port, and not ultramodern itself. So the bitset is
default-constructed with every bit clear, and every source is "discard". The
struct's own defaults say what was intended -- requeue timer, RSP, SI and RDP;
drop VI, AI and PI -- and the port now installs them, which is both the fix and
a statement of the policy.

For most sources a discard is survivable because another message follows: VI
retrace comes again in sixteen milliseconds. For the RDP it is fatal. Rayman 2's
frame loop is a strict handshake with exactly one message per frame, and losing
one stops it permanently, because the next one is only produced by the frame the
lost message was supposed to permit.

This was a real latent defect and is fixed. It was not, however, what was
blocking.

### The idle thread is the pump of last resort

The RDP completion was measured being enqueued with the right queue and the
right message value, and then never dequeued by any of the three consumers. The
reason is that at that instant no game thread could reach one: the producer was
blocked waiting for the framebuffer slot the completion would return, every
other thread was parked, and the only thread still running was the idle thread
-- spinning in game code, calling nothing that drains.

libultra's idle thread is entitled to spin, because interrupts fire regardless
of what is running. ultramodern's cannot: `run_next_thread` throws if no thread
is runnable, so the idle thread must always be there, and while it is the only
runnable thread it is also the only possible pump.

The fix injects the same yield used for func_8008F344. It has to go in **both**
halves of the loop, and the second half is the one that matters:

    .L8000069C: lhu $v0, D_800250D8      ; reload the flag
    .L800006A4: bne $v0, $s0, .L800006A4 ; branch to itself, no reload
                jal func_8008F86C
                j   .L8000069C

Hooking only the outer loop moved the display-list count from 1 to 0. The inner
branch targets itself over a register the body never reloads, so once taken the
outer hook never runs again -- the pump was installed in the half that was not
running. With both halves pumped the count went from 1 to twenty-odd.

### Where it stands

**The game renders.** Twenty-two or twenty-three display lists reach RT64 in the
first second, produced by a frame loop that is genuinely turning: the trace
shows func_8008F554 releasing the framebuffer, func_8008EDC0 taking it and
posting the next list, and the frame body cycling through all four of its
indirect calls. That is the whole handshake working.

It then stalls, in func_80026F38, reached from the frame body's second indirect
call. That function is self-recursive and none of its callees is a spin by the
tool's reckoning, so the next round starts by probing its call sites rather than
assuming it is the same defect a fourth time.

## Who posts to D_800EFE68, and the wait that was hiding behind it

The question that opened this round had a clean answer. `D_800EFE68` is a
one-slot semaphore holding the free framebuffer, seeded once in func_8008F878.
The only thing that posts to it is **func_8008F554**, which the task thread runs
on command `0x3E7` -- and 0x3E7 is what `osSetEventMesg(OS_EVENT_DP, ...)`
registers, so the slot comes back on RDP completion and on nothing else. Read in
full, the frame handshake is:

    func_8008EDC0   takes the free slot, terminates the list, posts D_800CE2BC
    func_8008F378   takes that, submits the graphics task
    RDP finishes    -> dp_complete() -> 0x3E7 -> func_8008F554
    func_8008F554   osViSwapBuffer, then returns the slot to D_800EFE68

Two things were breaking it, and both were the busy-wait defect again.

The first is recorded in the previous section: the idle thread was the only
runnable thread at the moment the RDP completion arrived, and it drains nothing.
The RDP message was measured being enqueued with the right queue and value and
never dequeued by any consumer. Pumping **both halves** of the idle loop moved
the display-list count from one to twenty-odd.

The second was found by the thread sampler rather than by bisection, after
hand-placed probes had chased it through four levels of call chain without
pinning it: 147 of about 170 in-code samples sat in **func_8008FAF8**, which is

    while (D_800CE2C4 != 0) { }   // the display list queue must drain
    while (D_800EFE70 != 1) { }   // and a flag must be set

with no calls at all. Pumping it took the rate from a burst of twenty to a
steady thirty-five to forty display lists a second, touching sixty.

### Two blind spots in the tool, both now fixed

`tools/find_spin_loops.py` did not report func_8008FAF8, and understanding why
made it better:

  * **Branch targets that are function labels.** Both of its branches target
    `func_8008FAF8` itself, and the scanner only recognised local `.L` labels. A
    branch to the enclosing function is now treated as a branch to its first
    instruction.
  * **Addresses materialised before the load.** MIPS builds an address with
    lui/addiu and then dereferences a register, so the load carries no `%lo` for
    the scanner to see. Any `%lo` reference in the body now counts as touching
    that global, not only one attached to a load.

### One reasoned refinement that measurement rejected

The obvious improvement is to poll with a zero timeout in waits on the frame
path, since a millisecond of latency is a large fraction of a sixteen
millisecond frame. Measured over five runs each, peak display-list rates were
34, 37, 45, 15, 18 polling against 33, 36, 39, 50, 40 sleeping -- better on the
median and far steadier asleep. A hot poll takes CPU from the native renderer
and audio threads and loses more than the latency saves. The helper stays at one
millisecond, and the numbers are in the source so it is not "optimised" back.

### Where it stands

The port renders. Display lists reach RT64 continuously, peaking at 35-40 per
second and touching 60, from a frame loop whose whole handshake is working.

The rate is not yet steady, and the next candidates are already listed rather
than needing to be hunted: `find_spin_loops.py` ranks func_800393CC and
func_8003941C (a mutually recursive pair on D_800CC620/D_800CC628),
func_80090D00, func_800AA9AC and func_800AA9F8 above everything else. Each is
five instructions, waits on a global it never writes, and cannot reach a yield.

## Sixty frames a second

The port now renders continuously. Over a forty-second run: **1844 display
lists at a sustained 59-61 per second**, no crash, no hang. After about six
seconds of loading it locks to sixty and stays there.

Two things got it there.

### The spin-loop tool, made precise

The ranked list was 362 candidates of which about six were real, which is not a
list anybody can act on -- and acting on it blindly would have been worse than
useless. Every one of the top entries after the three already fixed turned out
to be a false positive: two linked-list walkers, a tree walker, a bounded
three-element copy, a float subtraction loop, and a clear of 0x25800 words.
Injecting a one-millisecond yield into that last one would have taken minutes
per call.

What separates a wait from a traversal is not what it reads but whether its exit
condition depends on something it advances. A traversal computes its branch from
a register carried across the back edge (`lw $s0, 0x14($s0)`); a counted loop
does the same through `addiu`. A genuine wait recomputes a fixed address from
lui/%lo, or tests the result of a call, and so has nothing loop-carried in the
chain its branch depends on.

`tools/find_spin_loops.py` now computes that: the loop-carried registers (read
before written within the body) and the backward dependency closure of the
branch's tested registers, and rejects the loop if they intersect. Two smaller
fixes came with it -- `swc1`/`sdc1` were missing from the store set, so a loop
that wrote its own float flag looked like a wait; and a loop whose back edge is
an unconditional `j`, or whose branch is a floating-point condition, yields no
register dependencies at all, so there is nothing to judge and it is skipped
rather than reported.

The list went 362 -> 86 -> **38**, and the top of it is now exactly the real
cases: the three already fixed, four entries in libultra routines that librecomp
replaces anyway, and main's two-second osGetTime wait -- which does terminate,
but held up every external event while it ran, and is now pumped too.

### osSpTaskYield and osSpTaskYielded

With the frame loop running, the port got about seven seconds in and faulted
writing 0xA4040010, SP_STATUS. func_80007C60 is three instructions,
`__osSpSetStatus(0x400)`, and SP_SET_SIG0 is what osSpTaskYield writes to ask
the running microcode to stop. Its neighbour func_80007C80 reads SP_STATUS,
tests the yielded bit, folds it into the caller's OSTask flags and clears it --
osSpTaskYielded. Both are reimplemented by librecomp, which they must be:
whether a task yielded is a question about ultramodern's task thread, not about
a register this port does not have.

Naming them took the port from seven seconds and a crash to forty seconds at a
locked sixty.

### What remains

Without a button press the game sits at its Controller Pak prompt and renders
nothing at all, so a player would see a black screen with no indication that
anything is wanted. The prompt's wait runs before its draw, which is consistent
with what the earlier trace showed, but "correct and invisible" is not good
enough and it is the next thing to look at.

The remaining diagnostic scaffolding is gone. Seven hooks are active and every
one is load-bearing: the section registration, four spin pumps, and main's time
wait. No instruction patches.

## The prompt was never broken

The previous section ended by saying the Controller Pak prompt renders nothing,
that a player would see a black screen with no indication input was wanted, and
that this was the next thing to fix. **All of that was wrong**, and it was wrong
in an instructive way.

The evidence for it was the display-list counter reading zero for the whole time
the game sat at the prompt. That is true, and it means what it says: the RDP is
doing no work. It was then taken to mean the screen is blank, which does not
follow. The game draws that screen with the **CPU**, writing pixels straight into
RDRAM, and submits no display list at all. RT64 presents it perfectly.

Dumping the framebuffer the VI is pointed at settles it -- 49,472 non-black
pixels of 67,200, stable frame to frame -- and capturing the window shows the
finished article: the forest background, "No Controller Pak found. The game will
not be saved.", "Insert a Controller Pak and press the A Button.", "Press START
to continue without saving." Exactly what the hardware shows.

Two instruments came out of this and both are kept, because between them they
answer a question no counter in this port could:

  * `RAYMAN2_FBPROBE` in src/rt64_context.cpp reports and dumps the framebuffer
    the VI is scanning out; `tools/fb_to_png.py` makes the dump viewable. It
    handles the two details that matter -- RDRAM is word-swapped, so byte i of a
    big-endian word lives at index i^3, and the format is RGBA5551 whose 5-bit
    channels must be scaled by bit replication rather than shifted.
  * `tools/grab_window.ps1` captures the actual window. What the game drew and
    what the player sees are different questions, and only the second one is the
    one that matters.

The framebuffer is 300x224, not 320x240 -- main sets 0x12C by 0xE0 after the
initial 0x140 by 0xF0 -- which is worth knowing, because reading the dump at the
wrong width produces a plausible-looking diagonal smear rather than an obvious
error.

## Phase 04 is met

The gate was "Ubisoft logo, then the attract sequence rendering recognisably".
The port boots, shows its Controller Pak prompt, takes a button press, and plays
the intro cinematic: a 3D seascape with cliffs, sky and water, carrying the
narration "RAYMAN, LOOK WHAT THE PIRATES HAVE DONE TO OUR WORLD..." and "A
PLANET OF ANGUISH AND PAIN, HAUNTED BY EVIL." Thirty seconds, 1274 display
lists, a sustained 58-61 per second, zero crashes.

Seven hooks remain and every one is load-bearing: the static section
registration and six spin pumps. No instruction patches. The remaining known
gaps are audio, which is phase 05 and deliberately stubbed in src/rsp.cpp, and
the input mapping, which is phase 06.
