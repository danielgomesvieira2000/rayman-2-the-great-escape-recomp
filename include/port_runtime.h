/* Runtime functions the PORT must supply, declared for the recompiled C.
 *
 * N64Recomp keeps a built-in list of libultra functions it refuses to
 * translate, because they touch privileged CPU state that has no meaning in a
 * recompiled program. It emits no body for them, but it still emits the calls,
 * on the understanding that something else defines them.
 *
 * Two different "something else"s exist, and the distinction matters:
 *
 *   - Functions in N64Recomp's `reimplemented_funcs` set are provided by
 *     librecomp -- osGetCount, __osDisableInt, the cache routines, and so on.
 *     Nothing is needed here for those.
 *
 *   - Functions in its `ignored_funcs` set are not provided by anyone. They are
 *     the port's responsibility. The three below are the ones Rayman 2 actually
 *     calls, established by compiling the generated C and collecting every
 *     unresolved reference.
 *
 * This header only DECLARES them, which is all phase 02 needs: the recompiled
 * code compiles and archives, and the references stay unresolved inside the
 * static library. Phase 03 must define them in src/ before anything links into
 * an executable, and the definitions are not free choices:
 *
 *   __osGetSR      Returns the cop0 Status register. The port has no such
 *                  register. Returning 0 claims interrupts are disabled and no
 *                  status bits are set; whether any caller depends on the real
 *                  value is a phase 04 question.
 *   __osSetSR      Writes cop0 Status. Discarding the write is almost certainly
 *                  right -- the interrupt routines the game relies on
 *                  (__osDisableInt / __osRestoreInt) are handled by librecomp.
 *   __osSetCompare Writes cop0 Compare, the timer-match register that schedules
 *                  the next counter interrupt. ultramodern drives timing
 *                  itself, so this needs to agree with however phase 03 wires
 *                  the VI/timer, not simply be dropped.
 *
 * A fourth joined them with the Controller Pak:
 *
 *   __osSiRawStartDma  Moves the 64-byte PIF RAM to or from the controller bus.
 *                  It is named on purpose, so that the game's own Controller
 *                  Pak filesystem is recompiled and runs, with the port
 *                  answering only the joybus transactions underneath it. See
 *                  src/si_pak.cpp and the note beside the symbol in
 *                  recomp/symbol_addrs.txt.
 *
 * It is force-included by scripts/build-recompiled-lib.sh (-include), because
 * the generated sources include only "recomp.h" and "funcs.h" and must never be
 * hand-edited.
 */

#ifndef RAYMAN2_PORT_RUNTIME_H
#define RAYMAN2_PORT_RUNTIME_H

#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

void __osGetSR_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSiDeviceBusy_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSetSR_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSetCompare_recomp(uint8_t* rdram, recomp_context* ctx);
void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx);

/* Diagnostic, called from a [[patches.hook]] injected into the recompiled code.
   Declared here because this header is force-included into every generated
   source, which is what makes the injected text compile. See
   src/debug_node.cpp. */
/* Draw distance: called from a [[patches.hook]] at the start of the game's
   guPerspective, where it scales the far-plane argument before the projection
   is built. See src/draw_distance.cpp. */
void rayman2_scale_draw_distance(uint8_t* rdram, recomp_context* ctx);

/* The other half of the widescreen culling fix: notes the projection matrix
   guPerspective just widened, so it can be narrowed again when the display
   list is handed to the renderer rather than immediately. */
void rayman2_record_projection_matrix(uint8_t* rdram, recomp_context* ctx);

void rayman2_debug_node(uint8_t* rdram, uint32_t node_addr);
void rayman2_debug_assert(uint8_t* rdram, uint32_t ra);
void rayman2_debug_site(uint8_t* rdram, uint32_t site);
void rayman2_debug_flag(uint8_t* rdram, uint32_t addr, uint32_t loaded);
void rayman2_debug_count(uint8_t* rdram, uint32_t site);
void rayman2_debug_struct(uint8_t* rdram, uint32_t addr, uint32_t words);
void rayman2_debug_text(uint8_t* rdram, uint32_t addr);

/* Defined in src/spin_yield.cpp. Injected into game spin loops that wait on a
   flag another thread must clear: ultramodern only reschedules and only
   delivers external events inside message-queue calls, so a spin that makes
   none of those deadlocks the whole game. See that file. */
void rayman2_yield_in_spin(uint8_t* rdram);

// Defined in src/register_sections.cpp; called from a hook on the boot thread
// so that it runs after librecomp's init() has cleared and repopulated the
// function map. See that file for why the runtime's own pass gets two of the
// three sections wrong.
void rayman2_register_static_sections(void);


#ifdef __cplusplus
}
#endif

#endif /* RAYMAN2_PORT_RUNTIME_H */
