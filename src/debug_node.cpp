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
#include <chrono>

// A newline in a string literal, spelled so that generated-source tooling
// cannot turn the escape into a real line break.
#define NL "\n"
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
    if (seen >= 64) {
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

// Count visits to a site rather than announcing the first one.
//
// rayman2_debug_site answers "was this reached", which is the right question
// for a linear boot path and the wrong one for a loop: the game's main loop is
// a pair of nested while loops, and what matters is which of its five points
// are still being reached and how often. A first-hit report says only that the
// loop was entered; a rate says whether it is turning, which arm it is in, and
// whether it has stopped.
extern "C" void rayman2_debug_count(uint8_t* rdram, uint32_t site) {
    (void)rdram;
    static uint32_t sites[64];
    static unsigned long long hits[64];
    static int used = 0;
    static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();

    // Log every one of the first calls verbatim, with a sequence number and a
    // millisecond timestamp.
    //
    // Summaries alone proved ambiguous: counts that stay at one with a quarter
    // second between visits fit both "the loop turned once and stopped" and
    // "each callee is taking a quarter second", and those call for opposite
    // investigations. A raw trace distinguishes them without inference.
    static unsigned long long seq = 0;
    static const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    if (seq < 20000) {
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[rayman2] trace %llu  %08X  +%lldms" NL, seq, site, ms);
    }
    ++seq;

    int i = 0;
    for (; i < used; ++i) {
        if (sites[i] == site) break;
    }
    if (i == used) {
        if (used == 64) return;
        sites[used] = site;
        hits[used] = 0;
        ++used;
        // Announce a site the first time it is seen. Without this a loop that
        // is entered once and then blocks reports nothing at all, because the
        // periodic summary below only runs on a later hit -- silence that reads
        // identically to "never reached".
        std::fprintf(stderr, "[rayman2] loop: first visit to %08X\n", site);
    }
    ++hits[i];

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(250)) {
        return;
    }
    last = now;
    std::fprintf(stderr, "[rayman2] loop:");
    for (int j = 0; j < used; ++j) {
        std::fprintf(stderr, "  %08X=%llu", sites[j], hits[j]);
    }
    std::fprintf(stderr, "\n");
}

// Dump a game-side structure, once per distinct address.
//
// func_800A3878 rejects the request with 0x1004 because the descriptor at
// D_800CF624[0] carries a capability count in its halfword at offset 6 that is
// too small for the bit being asked for. Two very different things produce
// that: a descriptor that was never populated, which reads as zeros, and one
// that was populated from data saying a genuinely small number. Reading the
// bytes distinguishes them; reasoning about the loader cannot.
extern "C" void rayman2_debug_struct(uint8_t* rdram, uint32_t addr, uint32_t words) {
    static uint32_t seen[8];
    static int used = 0;
    for (int i = 0; i < used; ++i) {
        if (seen[i] == addr) return;
    }
    if (used == 8) return;
    seen[used++] = addr;

    if (addr == 0) {
        std::fprintf(stderr, "[rayman2] struct: NULL" NL);
        return;
    }
    const gpr base = (gpr)(int32_t)addr;
    std::fprintf(stderr, "[rayman2] struct at 0x%08X:" NL, addr);
    for (uint32_t i = 0; i < words; i += 4) {
        std::fprintf(stderr, "[rayman2]   +0x%02X: %08X %08X %08X %08X" NL,
                     i * 4,
                     (uint32_t)MEM_W(i * 4 + 0x0, base), (uint32_t)MEM_W(i * 4 + 0x4, base),
                     (uint32_t)MEM_W(i * 4 + 0x8, base), (uint32_t)MEM_W(i * 4 + 0xC, base));
    }
}

// Print a NUL-terminated game string, once per distinct address.
//
// The game formats its own messages through func_80090BB8, which fetches the
// text for an id and then walks it. Reading that text is worth more than
// another round of bisection: when a program stops to tell the player what is
// wrong, the fastest way to find out what is wrong is to read what it says.
//
// Bytes are fetched one at a time through MEM_BU rather than by casting a
// pointer, because RDRAM stores each 32-bit word in host order -- byte i of a
// big-endian word lives at index i^3, which the macro handles and a memcpy
// would silently get backwards.
extern "C" void rayman2_debug_text(uint8_t* rdram, uint32_t addr) {
    static uint32_t seen[16];
    static int used = 0;
    if (addr == 0) return;
    for (int i = 0; i < used; ++i) {
        if (seen[i] == addr) return;
    }
    if (used == 16) return;
    seen[used++] = addr;

    const gpr base = (gpr)(int32_t)addr;
    char buf[97];
    int n = 0;
    for (; n < 96; ++n) {
        const uint8_t ch = (uint8_t)MEM_BU(n, base);
        if (ch == 0) break;
        buf[n] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    buf[n] = '\0';
    std::fprintf(stderr, "[rayman2] text 0x%08X: \"%s\"" NL, addr, buf);
}
