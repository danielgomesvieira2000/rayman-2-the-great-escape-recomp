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

## Still outstanding

- **Nineteen functions in the boot segment poke hardware registers** (a scan for
  `lui` of the register banks finds them). Four are now named or accounted for.
  The rest are ahead, and most should disappear as their public callers get
  named rather than needing individual attention.
- **The audio microcode** is still not recompiled, so `src/rsp.cpp` reports
  audio tasks complete without synthesising anything. That is phase 05.
- **`__osGetSR` returning 0** remains an unexamined approximation now that real
  game code is executing.
