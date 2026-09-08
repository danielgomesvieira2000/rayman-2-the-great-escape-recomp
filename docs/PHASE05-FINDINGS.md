# Phase 05 — Graphics and audio correctness

## The graphics microcode, confirmed

Phase 00 identified the renderer's microcode as F3DEX.NoN 1.23 by inspection and
annotated the reading "needs confirmation". It is confirmed, twice over.

The cartridge carries exactly one microcode identification string:

    ROM 0x01CD10   RSP Gfx ucode F3DEX.NoN     1.23 Yoshitaka Yasumoto Nintendo.

and it sits inside the data blob that the graphics task actually points at --
0x8001BE60, the string at +0x2B0 -- rather than merely somewhere in the ROM.
Logging the OSTask handed to `send_dl` shows RT64 being given
`ucode=0x80018C80`, whose data is that blob. So the microcode the renderer
dispatches is the one whose version string this is, which is what the annotation
was asking for.

## The microcode layout

Both microcodes live in the boot segment and share a boot stub. The task
structures give the addresses; the *sizes* in them are nominal and overlap, so
the real extents come from where the next block starts:

    0x80017D90   ucode_boot, shared           0x00D0
    0x80017E60   audio text                   0x0E20   (declared 0x1000)
    0x80018C80   graphics text, F3DEX.NoN 1.23
    0x8001BBA0   audio data                   0x02C0   (declared 0x0800)
    0x8001BE60   graphics data                        (version string at +0x2B0)

The boot segment maps ROM 0x1000 to VRAM 0x80000400, so the audio text is at ROM
0x18A60.

Taking the declared 0x1000 at face value walked RSPRecomp off the end of the
audio microcode and into F3DEX, where it stopped on `mfc0 $rt, $10` at +0xEEC --
reported as "Unhandled mfc0: 10", which reads like an unsupported instruction
and is really a wrong length. Anything that recompiles a fixed extent out of a
ROM has this failure mode: the error names the first thing it could not parse,
not the reason it was parsing there.

## Audio works

`scripts/recompile-rsp.sh` runs RSPRecomp from `recomp/rsp_audio.us.toml` and
produces `RecompiledRsp/rsp_audio.cpp`, whose entry point matches librecomp's
`RspUcodeFunc` signature. `src/rsp.cpp` dispatches it on the microcode *address*
rather than the task type, because the type says what the game means and the
address says which code is about to run.

Three things had to be right, and the first two were wrong in ways that looked
like something else.

### The microcode is not loaded at the start of IMEM

This was the whole of the first failure, and it was settled by disassembling the
0xD0-byte boot stub rather than by reasoning about the microcode. rspboot is
itself loaded at IMEM 0x1000 and does:

    0x1008  lw   $v0, 0x10($at)        ; task.ucode      ($at = 0xFC0)
    0x100C  addi $v1, $zero, 0xF7F     ; length - 1
    0x1010  addi $a3, $zero, 0x1080    ; IMEM destination
    0x1014  mtc0 $a3, SP_MEM_ADDR
    0x1018  mtc0 $v0, SP_DRAM_ADDR
    0x101C  mtc0 $v1, SP_RD_LEN
    0x1034  jr   $a3                   ; and enter it there

So the microcode is loaded at **0x04001080** and entered there, leaving the
first 0x80 bytes of IMEM to the boot stub. Recompiled as though it began at
0x04001000, every label sat 0x80 bytes away from the address the jumps actually
use. The symptoms were thoroughly misleading: the first `jal` landed inside a
DMA helper, and the command loop then read what looked like an audio command out
of DMEM and dispatched on it, producing an indirect jump to 0xFFFFF000. Reading
that back it was tempting to conclude the command list had never been DMA'd --
a plausible story about a real mechanism, and entirely wrong. The addresses were
simply shifted.

### The dispatch table is data, so no static scan can find it

With the load address fixed the jump target became 0x12D0 -- a real address
inside the microcode, which is what a correct-control-flow failure looks like.
The microcode reads a command word from DMEM, takes the index from bits 30..24
doubled, and `jr`s through a table of sixteen halfwords at `ucode_data + 0x10`.
Those are data, so RSPRecomp cannot reach them by following branches; they are
listed in the config as `extra_indirect_branch_targets`. All sixteen land inside
the microcode's IMEM range of 0x1080..0x1EA0, which is the check that they are
really code addresses and not a misread of the data.

### The two audio callbacks use different units

With the microcode running, sound came out but the output queue grew by about
sixteen thousand frames a second, so the audio drifted steadily behind the
picture. The cause is an asymmetry in ultramodern's audio callbacks that is easy
to miss:

  * `queue_samples(int16_t*, size_t count)` receives `byte_count / sizeof(int16_t)`
    -- a count of **int16 values**, counting each channel separately.
  * `get_frames_remaining()` is multiplied by `2 * sizeof(int16_t)` on the way
    back, so that one is **frames**.

The port multiplied the first by bytes-per-frame, queueing twice as many bytes
as the game produced and reading past the end of the buffer. Corrected, the
production rate matches the device exactly.

### Result

The game requests 48000 Hz at start-up and then 22050 Hz. Measured over
twenty-five seconds at 22050:

    audio 22400 frames/s  peak 13264  queued 32
    audio 22400 frames/s  peak 15096  queued 928
    audio 22240 frames/s  peak 12902  queued 736
    audio 22080 frames/s  peak 16552  queued 416

Production matches the device rate, the queue stays within a thousand frames --
a few tens of milliseconds of latency, not growing -- and the peak moves with
the content rather than sitting at zero or clipping. Forty-five seconds with
audio enabled: 2152 display lists at a sustained 60 a second, no failed tasks,
no unhandled jumps, and the intro cinematic still rendering correctly.

`RAYMAN2_AUDIOPROBE=1` reports that rate/peak/queue line once a second. It is
worth keeping: "samples are being queued" and "there is sound" are different
claims, and a microcode that runs to completion writing silence produces exactly
the same frame count as one that works.

## Still to do for the gate

The gate is the first three levels playing start to finish with correct visuals,
audio and Controller Pak saves. Remaining: the fog and transparent water that
phase 00 flagged as the usual casualties under an HLE renderer, and F3DEX 1.x
command-level behaviour.

The Controller Pak is done. It is emulated at the joybus level, underneath the
cartridge's own recompiled filesystem, so what the port stores is a real pak
image rather than an approximation of one. See
[CONTROLLER-PAK-FINDINGS.md](CONTROLLER-PAK-FINDINGS.md).
