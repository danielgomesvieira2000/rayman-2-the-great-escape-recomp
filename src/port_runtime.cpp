// The libultra routines N64Recomp drops and leaves to the port.
//
// N64Recomp splits the libultra functions it refuses to translate into two
// groups. Functions in `reimplemented_funcs` are supplied by librecomp and
// nothing is needed here. Functions in `ignored_funcs` are supplied by nobody:
// the recompiler emits the calls and expects the port to define them. Phase 02
// established, by compiling the generated C and collecting every unresolved
// reference, that Rayman 2 calls exactly three of those.
//
// See include/port_runtime.h for the declarations and the reasoning; this file
// is where the reasoning turns into behaviour.

#include <atomic>
#include <cstdint>

#include "recomp.h"
#include "port_runtime.h"

namespace {
    // Last value the game wrote to cop0 Compare. Kept so the value is
    // observable while bringing the port up, and so a future timer
    // implementation has somewhere to read the game's intent from.
    std::atomic<uint32_t> g_compare{0};
}

extern "C" {

// Reads cop0 Status.
//
// There is no Status register in a recompiled program. Returning 0 reports
// interrupts disabled with no status bits set.
//
// This is the least certain of the three, and it is deliberately the first
// suspect if early execution goes astray. What makes it defensible is that the
// routines the game actually relies on for critical sections -- __osDisableInt
// and __osRestoreInt -- are reimplemented by librecomp against its own thread
// model rather than routed through here, so a caller reading Status directly is
// doing something the runtime does not model in the first place.
void __osGetSR_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->r2 = 0;   // $v0
}

// Writes cop0 Status.
//
// Discarding the write is right for the same reason: librecomp owns interrupt
// state, and letting the game's idea of Status reach it would be worse than
// ignoring it.
void __osSetSR_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    (void)ctx;     // $a0 is the value to write; the port has nowhere to put it
}

// Writes cop0 Compare, the timer-match register that schedules the next counter
// interrupt.
//
// This one is NOT simply droppable in principle: on hardware it is half of the
// mechanism that drives periodic timing. In this port it is inert because
// ultramodern schedules the game's timers itself -- osSetTimer and the VI event
// loop -- so the counter interrupt this would arm is never the thing that wakes
// anything. The value is recorded rather than discarded so that if phase 04
// turns up a timing dependency, the game's intent is already available here
// instead of having to be recovered.
void __osSetCompare_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    g_compare.store(static_cast<uint32_t>(ctx->r4), std::memory_order_relaxed);  // $a0
}

// Reports whether the Serial Interface is still busy with a transfer.
//
// On hardware this spins while the SI moves 64 bytes to or from PIF RAM. In the
// port there is no such transfer to wait for: controller state is answered
// synchronously by the runtime, so the SI is never busy and returning anything
// else would spin the caller forever.
//
// This is one of N64Recomp's `ignored_funcs` -- dropped by the recompiler and
// left for the port -- which is why it is here rather than in librecomp. The
// game reaches it during early boot, poking SI_STATUS (0xA4800018) directly
// rather than going through the libultra wrappers.
void __osSiDeviceBusy_recomp(uint8_t* /*rdram*/, recomp_context* ctx) {
    ctx->r2 = 0;   // $v0 -- never busy
}

} // extern "C"

namespace rayman2 {
    uint32_t last_compare_value() {
        return g_compare.load(std::memory_order_relaxed);
    }
}
