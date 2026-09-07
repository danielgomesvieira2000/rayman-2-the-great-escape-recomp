// Report the list node whose field trips the game's own assertion.
//
// Phase 04 traced the boot failure to a serialiser walking a linked list: it
// requires the field at 0xC of each node to fit in a byte, because it is about
// to store it with `sb`, and one node's does not. Everything up to that point
// was recoverable from the disassembly; the value itself is not, and neither is
// the address it came from.
//
// This is called from a `[[patches.hook]]` in recomp/rayman2.us.toml, which
// injects a line of C into the recompiled function immediately before the
// check. That mechanism turned out to be far lighter than expected: N64Recomp's
// FunctionTextHook splices literal text into its own output, so no MIPS
// cross-compilation is involved and none of the `patches/` build is needed.
//
// Reading node fields needs the recompiler's memory macros, which resolve a
// game address against a variable that must be named `rdram` -- hence the
// parameter name. The address arrives as the raw 32-bit register value and is
// sign-extended here, for the same reason the entrypoint had to be: KSEG0
// 0x80xxxxxx is 0xFFFFFFFF80xxxxxx to those macros, and a zero-extended value
// lands 4 GB away.

#include <cstdint>
#include <cstdio>

#include "recomp.h"
#include "port_runtime.h"

namespace {
    // The list is walked repeatedly and the assert fires in a loop, so this
    // would otherwise produce an unbounded stream. A handful of nodes is enough
    // to tell "one bad entry" from "the whole list is garbage".
    constexpr int kMaxReports = 24;
    int g_reports = 0;
}

extern "C" void rayman2_debug_node(uint8_t* rdram, uint32_t node_addr) {
    if (g_reports >= kMaxReports) {
        return;
    }
    ++g_reports;

    if (node_addr == 0) {
        std::fprintf(stderr, "[rayman2] assert node: NULL\n");
        return;
    }

    const gpr node = (gpr)(int32_t)node_addr;

    // Offsets established by tracing func_80088B00 and func_80088B5C:
    //   0x0  next pointer
    //   0x8  a pointer selected by the flag bits
    //   0xC  the field that must fit in a byte
    //   0x10 type tag in the top six bits, flags below
    const uint32_t next  = (uint32_t)MEM_W(0x0, node);
    const uint32_t ptr8  = (uint32_t)MEM_W(0x8, node);
    const uint32_t field = (uint32_t)MEM_W(0xC, node);
    const uint16_t flags = (uint16_t)MEM_HU(0x10, node);

    std::fprintf(stderr,
                 "[rayman2] assert node=0x%08X next=0x%08X ptr@8=0x%08X "
                 "field@C=0x%08X flags@10=0x%04X type=0x%04X %s\n",
                 node_addr, next, ptr8, field, flags,
                 (uint16_t)(flags & 0xFC00),
                 field < 0x100 ? "(fits)" : "(TOO BIG)");
}

// Which of the ten call sites actually reaches the assert routine?
//
// The first attempt hooked the call site traced from the disassembly, and it
// never fired -- consistent with an earlier result that had been noticed and
// under-weighted: NOPing the `jal` at that site did not stop the break, which
// already proved the firing site was a different one. The break itself reports
// only the routine's own address, so the caller has to come from the return
// address, which at function entry is in $ra (register 31).
extern "C" void rayman2_debug_assert(uint8_t* rdram, uint32_t ra) {
    static int seen = 0;
    if (seen >= 12) {
        return;
    }
    ++seen;
    // $ra holds the address AFTER the delaying jal, so the call is at ra - 8.
    std::fprintf(stderr, "[rayman2] ASSERT reached from 0x%08X (call at 0x%08X)\n",
                 ra, ra - 8);
}

// Report an assert call site by its own address, one hook per site.
//
// The routine-entry hook found $ra == 0, so the assert is not reached by an
// ordinary jal, and the node hook on the site the disassembly trace picked out
// never fired. Rather than reason further about which of the ten it is, every
// site announces itself.
extern "C" void rayman2_debug_site(uint8_t* rdram, uint32_t site) {
    static int seen = 0;
    if (seen >= 12) {
        return;
    }
    ++seen;
    std::fprintf(stderr, "[rayman2] ASSERT fired at call site 0x%08X\n", site);
}

// Is the boot segment's bss actually zeroed?
//
// The flag at 0x800250D8 is READ in exactly one place and written nowhere in
// any of the three segments. It lives in the boot segment's bss, which the
// entry stub clears (0x8001D0C0 for 0x8B90 bytes), so it should read 0 and the
// boot thread should spin on it. It reads 1, so something is wrong with that
// assumption -- and the way to find out which is to look at the memory rather
// than reason about it.
//
// If the surrounding words are zero, the clear ran and something set this one.
// If they carry plausible ROM data, the clear did not cover this address and
// what is being read is leftover from the 1 MB boot DMA.
extern "C" void rayman2_debug_flag(uint8_t* rdram, uint32_t addr, uint32_t loaded) {
    static int seen = 0;
    if (seen >= 3) {
        return;
    }
    ++seen;

    const gpr base = (gpr)(int32_t)(addr & ~0xFu);
    std::fprintf(stderr, "[rayman2] flag 0x%08X reads %u; memory around it:\n",
                 addr, loaded);
    for (int row = -2; row <= 2; ++row) {
        const gpr a = base + (gpr)(int32_t)(row * 16);
        std::fprintf(stderr,
                     "[rayman2]   0x%08X: %08X %08X %08X %08X\n",
                     (uint32_t)(addr & ~0xFu) + row * 16,
                     (uint32_t)MEM_W(0x0, a), (uint32_t)MEM_W(0x4, a),
                     (uint32_t)MEM_W(0x8, a), (uint32_t)MEM_W(0xC, a));
    }
}
