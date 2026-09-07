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

The threading and message-queue layer is now the runtime's, and boot has moved
past it: the current fault is at offset `0x24400010` from RDRAM, which is
`0xA4400010` -- **VI_CURRENT**. Back to a hardware register, and out of the
null-pointer class entirely.

That is the frontier: the Video Interface. `func_80011480` reads VI_CURRENT and
`func_80008300` writes the whole VI register block, so the next names to prove
are around `osViGetCurrentLine`, `osViSetMode`/`osViSwapBuffer` and the VI
manager. The public/private rule from earlier in this phase applies again --
`osViSwapBuffer` is reimplemented while `__osViSwapContext` is not, so the win
is naming the public entry point and letting the manager beneath it go away.

Still no display lists, so nothing renders yet.

So execution is proceeding and **nothing is reaching the renderer**. The next
question is which of these it is, and they are distinguishable:

1. The game is stuck in an early wait loop — blocked on a message queue, a VI
   retrace or a DMA completion that never arrives — and has not got as far as
   drawing. The steady but modest CPU use is consistent with a spin.
2. The game is running its main loop and submitting graphics tasks, but they are
   not reaching `send_dl` in `src/rt64_context.cpp`.
3. Display lists arrive and RT64 draws nothing recognisable.

**The cheapest way to tell them apart is a counter.** `src/rt64_context.cpp`
should log the first call to `send_dl` and `send_dummy_workload`, and how many
of each arrive per second. That single measurement splits (1) from (2) from (3),
and the phase 03 lesson applies: build the instrument before forming the theory.

## Still outstanding

- **Nineteen functions in the boot segment poke hardware registers** (a scan for
  `lui` of the register banks finds them). Four are now named or accounted for.
  The rest are ahead, and most should disappear as their public callers get
  named rather than needing individual attention.
- **The audio microcode** is still not recompiled, so `src/rsp.cpp` reports
  audio tasks complete without synthesising anything. That is phase 05.
- **`__osGetSR` returning 0** remains an unexamined approximation now that real
  game code is executing.
