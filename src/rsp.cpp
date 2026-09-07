// RSP microcode dispatch.
//
// The N64's signal processor runs two kinds of task, and only one of them
// reaches this file.
//
// GRAPHICS tasks never do: RT64 intercepts the display list before the task is
// ever handed to the RSP and renders it on the host GPU. Phase 00 established
// this game uses stock F3DEX.NoN 1.23, which RT64 already has a GBI table entry
// for, so nothing needs recompiling for it.
//
// AUDIO tasks do reach here, and Rayman 2's audio microcode has not been put
// through RSPRecomp yet. Returning a function that reports a normal RSP break
// makes librecomp's run_task() treat the task as completed rather than exiting
// the task thread, so the game keeps running with no sound. Returning nullptr
// instead would print an error and terminate the program, which would make it
// impossible to reach the phase 03 gate over a missing microcode that phase 05
// is scheduled to deal with.
//
// The silence is therefore deliberate and temporary, and it is loud in the log
// the first time it happens so it cannot be mistaken for the audio path working.

#include <cstdint>
#include <cstdio>

#include "librecomp/rsp.hpp"

namespace {
    bool g_warned_unhandled = false;

    // Reports the task as finished without doing anything. See the note above.
    RspExitReason silent_task_stub(uint8_t* /*rdram*/, uint32_t /*ucode_addr*/) {
        return RspExitReason::Broke;
    }
}

RspUcodeFunc* rayman2_get_rsp_microcode(const OSTask* task) {
    if (!g_warned_unhandled) {
        g_warned_unhandled = true;
        std::fprintf(stderr,
                     "[rayman2] RSP task type %u has no recompiled microcode; "
                     "reporting it complete and continuing without audio. "
                     "Generating the audio ucode with RSPRecomp is phase 05 work.\n",
                     task ? static_cast<unsigned>(task->t.type) : 0u);
    }
    return silent_task_stub;
}
