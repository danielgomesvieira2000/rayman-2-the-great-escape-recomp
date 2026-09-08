// Draw distance: scale the far plane the game asks guPerspective for.
//
// func_800038C0 is libultra's guPerspective, identified from its arithmetic and
// not from a call site. It multiplies fovy by a DOUBLE-precision PI/360 (which
// is why searching the ROM for a single-precision PI constant finds nothing),
// takes sine and cosine of the half angle and divides them for the tangent, and
// then builds the projection's depth terms as (n+f)/(n-f) and 2nf/(n-f) with a
// -1.0f in the [2][3] slot. Nothing else looks like that.
//
// Its signature is the libultra one:
//
//     guPerspective(Mtx *m, u16 *perspNorm, float fovy, float aspect,
//                   float near, float far, float scale)
//
// Under the o32 ABI the first four arguments arrive in a0..a3 -- the two floats
// as raw bit patterns in integer registers, which is why the prologue does
// `mtc1 $a2, $f20` -- and near, far and scale are passed on the stack. The
// function allocates 0x88 and then reads them at 0x98, 0x9C and 0xA0, so from
// the CALLER's stack pointer they are at 0x10, 0x14 and 0x18. The hook that
// calls this runs at_func_start, before the prologue, so ctx->r29 is still the
// caller's stack pointer and those three offsets are the arguments.
//
// Scaling `far` here rather than editing the finished matrix is deliberate:
// guPerspective also derives perspNorm from near and far, and the game hands
// that to the RSP. Changing the input keeps the two consistent; changing the
// output would not.
//
// WHAT THIS CANNOT DO. A far plane decides what the projection keeps. It does
// not decide what the game bothers to submit -- if Rayman 2 rejects distant
// objects on the CPU before building a display list, they are already gone by
// the time this runs, and no far plane will bring them back. That is the same
// unknown as docs/issues/001, and it is why this ships behind a measurement
// rather than an assumption.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "recomp.h"
#include "port_runtime.h"

namespace {

std::atomic<float> g_scale{1.0f};
std::atomic<bool> g_from_env{false};

float current_scale() {
    static const bool checked = []() {
        if (const char* value = std::getenv("RAYMAN2_DRAWDIST")) {
            const float parsed = static_cast<float>(std::atof(value));
            if (parsed >= 1.0f && parsed <= 16.0f) {
                g_scale.store(parsed, std::memory_order_relaxed);
                g_from_env.store(true, std::memory_order_relaxed);
            }
        }
        return true;
    }();
    (void)checked;
    return g_scale.load(std::memory_order_relaxed);
}

} // namespace

namespace rayman2 {

// Set from the frontend. Ignored while RAYMAN2_DRAWDIST is pinning a value, so
// that a measurement run cannot be quietly overridden by a saved setting.
void set_draw_distance_scale(float scale) {
    if (!g_from_env.load(std::memory_order_relaxed) && scale >= 1.0f && scale <= 16.0f) {
        g_scale.store(scale, std::memory_order_relaxed);
    }
}

float draw_distance_scale() {
    return current_scale();
}

} // namespace rayman2

namespace {

// Report what the hook is actually seeing, a few times, under RAYMAN2_DDPROBE.
//
// The point is to check the argument reading rather than the effect: if the
// aspect comes back as roughly 1.333 and near and far look like a plausible
// pair of distances, the o32 stack offsets above are right. If they come back
// as noise, everything downstream of them is worthless no matter how the
// picture looks.
void probe(float fovy, float aspect, float near_plane, float far_plane, float scaled) {
    static const bool on = std::getenv("RAYMAN2_DDPROBE") != nullptr;
    if (!on) {
        return;
    }
    static int remaining = 8;
    if (remaining-- <= 0) {
        return;
    }
    std::fprintf(stderr, "[rayman2] guPerspective fovy=%.3f aspect=%.4f near=%.3f far=%.3f -> far=%.3f\n",
                 fovy, aspect, near_plane, far_plane, scaled);
}

float bits_to_float(int32_t word) {
    float value = 0.0f;
    const uint32_t bits = static_cast<uint32_t>(word);
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

extern "C" void rayman2_scale_draw_distance(uint8_t* rdram, recomp_context* ctx) {
    const float scale = current_scale();
    const int64_t caller_sp = ctx->r29;

    uint32_t bits = static_cast<uint32_t>(MEM_W(0x14, caller_sp));
    float far_plane = 0.0f;
    std::memcpy(&far_plane, &bits, sizeof(far_plane));

    probe(bits_to_float(static_cast<int32_t>(ctx->r6)),
          bits_to_float(static_cast<int32_t>(ctx->r7)),
          bits_to_float(MEM_W(0x10, caller_sp)),
          far_plane, far_plane * scale);

    // Leave anything that is not a sane positive distance alone. A projection
    // built from a garbage far plane is worse than a short draw distance.
    if (scale <= 1.0f || !(far_plane > 0.0f) || far_plane > 1.0e9f) {
        return;
    }

    far_plane *= scale;
    std::memcpy(&bits, &far_plane, sizeof(bits));
    MEM_W(0x14, caller_sp) = static_cast<int32_t>(bits);
}
