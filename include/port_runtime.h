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

/* Diagnostic, called from a [[patches.hook]] injected into the recompiled code.
   Declared here because this header is force-included into every generated
   source, which is what makes the injected text compile. See
   src/debug_node.cpp. */
void rayman2_debug_node(uint8_t* rdram, uint32_t node_addr);
void rayman2_debug_assert(uint8_t* rdram, uint32_t ra);
void rayman2_debug_site(uint8_t* rdram, uint32_t site);
void rayman2_debug_flag(uint8_t* rdram, uint32_t addr, uint32_t loaded);
void rayman2_debug_count(uint8_t* rdram, uint32_t site);

// Defined in src/register_sections.cpp; called from a hook on the boot thread
// so that it runs after librecomp's init() has cleared and repopulated the
// function map. See that file for why the runtime's own pass gets two of the
// three sections wrong.
void rayman2_register_static_sections(void);


#ifdef __cplusplus
}
#endif

#endif /* RAYMAN2_PORT_RUNTIME_H */
