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

## Where the audio stands

`scripts/recompile-rsp.sh` runs RSPRecomp from `recomp/rsp_audio.us.toml` and
produces `RecompiledRsp/rsp_audio.cpp` -- 2245 lines, entry point
`rayman2_rsp_audio`, matching librecomp's `RspUcodeFunc` signature exactly. It
compiles and links, and `src/rsp.cpp` dispatches it on the microcode *address*
rather than the task type, because the type says what the game means and the
address says which code is about to run.

Two build details were needed. The generated file is C++ -- it includes
librecomp's rsp headers and uses attributes and value initialisation -- so it is
named `.cpp`; naming it `.c` and letting CMake infer the language does not work.
And it needs `-msse4.1`, because librecomp implements the RSP's vector unit with
x86 intrinsics and several of them reach for `_mm_blendv_epi8`, which is an
`always_inline` and so fails the build rather than falling back. That flag is set
on the one translation unit, not the executable: every other object here is
either generated MIPS-to-C, which gains nothing, or host code where raising the
instruction-set floor would be a portability decision made by accident.

**It runs, and it does not yet work.** It stops with `UnhandledJumpTarget` on an
indirect jump at the top of the command loop. What is established:

  * The dispatch reads a command word from DMEM 0x380 and takes the index from
    bits 30..24, doubled: `r1 = (word >> 23) & 0xFE`.
  * At the failure that word is `0x109C156C`, so the index is 0x20 -- entry 16
    of a table that has sixteen entries, at ucode_data +0x10..+0x2F. One past
    the end, and the halfword it reads instead is 0xF000, which sign-extends to
    the 0xFFFFF000 the jump reports.
  * That word is what `ucode_data` itself contains at +0x380. librecomp's
    `run_task` seeds DMEM by copying the OSTask to 0xFC0 and DMA-ing
    `ucode_data` to 0x0000, so DMEM 0x380 holds microcode data, not a command
    list. The microcode is dispatching on its own constants because the audio
    command list has not been brought in yet.

So the question for the next round is what should have loaded the command list
into DMEM before the loop reads it, and whether the entry point or the initial
register state differs from what the real boot stub leaves behind. The table at
ucode_data +0x10 is not a command table -- its first entry, 0x1118, is the
microcode's own DMA-read helper -- so it is a table of routines, which is worth
knowing before reading the dispatch again.

Until that is settled the recompiled microcode is **opt-in**, behind
`RAYMAN2_RSP_AUDIO=1`. librecomp treats any exit other than `Broke` as a failed
task and ends the program, so dispatching it by default would trade a port that
runs at sixty frames a second without sound for one that dies after two seconds.
The default remains the silent stub.
