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
#include <mutex>
#include <vector>

#include "ultramodern/config.hpp"

#include "recomp.h"
#include "port_runtime.h"

namespace {

float bits_to_float(int32_t word) {
    float value = 0.0f;
    const uint32_t bits = static_cast<uint32_t>(word);
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

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


} // namespace

namespace {

// Scale the aspect ratio the game builds its projection from.
//
// This is the experiment for docs/issues/001. RT64 widens the view on its own
// side of the display list, so the game goes on culling against the 1.3393 it
// asks for here, and the strips widescreen adds are drawn from geometry the
// game already discarded -- visible in the intro as a hard vertical seam at the
// right edge of the original 4:3 frame.
//
// aspect arrives in $a3 as a float bit pattern, and the hook runs before the
// prologue does `mtc1 $a3, $f22`, so writing ctx->r7 changes what the function
// computes with. What that proves depends on what happens to the picture:
//
//   the seam goes and the framing is unchanged  -> culling follows this aspect
//                                                  and rendering does not: done
//   the seam goes and the view widens again     -> both follow it, and RT64's
//                                                  own widening has to be
//                                                  turned off to compensate
//   the seam stays                              -> the culling is somewhere
//                                                  else entirely
struct Pending {
    uint32_t address;
    float factor;
};

std::mutex g_pending_mutex;
std::vector<Pending> g_pending;

std::atomic<uint32_t> g_perspective_mtx{0};
std::atomic<float> g_perspective_k{1.0f};
std::atomic<float> g_display_aspect{0.0f};
std::atomic<float> g_observed_aspect{300.0f / 224.0f};

// The aspect the game asked for the very first time, before anything here
// touched it. Everything is measured against this rather than against whatever
// arrived on the current call, and that is the whole correction: the previous
// version multiplied what it was given, the game hands back what it was given,
// and the value climbed every frame until it hit a clamp.
std::atomic<float> g_base_aspect{0.0f};

// What this call's aspect should BE. An absolute target, so handing the game a
// value it will hand back next frame is harmless -- the answer is the same
// every time.
float aspect_target_for(float asked) {
    static const float forced = []() {
        if (const char* env = std::getenv("RAYMAN2_ASPECT")) {
            const float parsed = static_cast<float>(std::atof(env));
            return (parsed >= 0.25f && parsed <= 4.0f) ? parsed : 0.0f;
        }
        return 0.0f;
    }();

    const float base = g_base_aspect.load(std::memory_order_relaxed);
    if (forced > 0.0f && base > 0.0f) {
        return base * forced;
    }

    const float display = g_display_aspect.load(std::memory_order_relaxed);
    if (!(display > 0.0f) || !(asked > 0.0f)) {
        return asked;
    }

    // Never narrow. A camera that genuinely wants a wider view than the display
    // keeps it; only a narrower one is opened out.
    return (display > asked) ? display : asked;
}

} // namespace

extern "C" void rayman2_scale_draw_distance(uint8_t* rdram, recomp_context* ctx) {
    const float scale = current_scale();
    const int64_t caller_sp = ctx->r29;

    const float asked_aspect = bits_to_float(static_cast<int32_t>(ctx->r7));
    if (asked_aspect > 0.1f && asked_aspect < 10.0f) {
        g_observed_aspect.store(asked_aspect, std::memory_order_relaxed);

        // The first value seen is the game's own, and is kept forever as the
        // reference. Nothing after this point can move it, so nothing can drift.
        float expected = 0.0f;
        g_base_aspect.compare_exchange_strong(expected, asked_aspect, std::memory_order_relaxed);

        const float base = g_base_aspect.load(std::memory_order_relaxed);
        const float target = aspect_target_for(asked_aspect);

        if (base > 0.0f && target > base * 1.001f) {
            uint32_t aspect_bits = 0;
            std::memcpy(&aspect_bits, &target, sizeof(aspect_bits));
            ctx->r7 = static_cast<int32_t>(aspect_bits);

            // The factor to undo at the handover is measured against the base,
            // not against what arrived -- what arrived may already be ours.
            g_perspective_mtx.store(static_cast<uint32_t>(ctx->r4), std::memory_order_relaxed);
            g_perspective_k.store(target / base, std::memory_order_relaxed);
        }
    }

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

// ---------------------------------------------------------------------------
// Widescreen: which layer does the widening
// ---------------------------------------------------------------------------

namespace rayman2 {

// Widen what the game CULLS against, and nothing else.
//
// Those two produce the same framing and are not the same thing. RT64's Expand
// widens the view on its own side of the display list, so the game goes on
// culling against its 4:3 frustum and the strips widescreen adds are drawn from
// geometry the game already threw away -- the seam in the opening cinematic,
// docs/issues/001. Widening the game's own projection instead means its frustum
// IS the widened one, and whatever it culls against follows.
//
// The two cannot both do it: each multiplies the horizontal field of view, and
// together they overshoot by a third. So when the player asks for Expand the
// port hands the game the display's aspect and tells RT64 not to expand, which
// leaves the game drawing an anamorphic wide view into its 4:3 framebuffer --
// exactly what a widescreen hack on real hardware does -- and sets the
// presentation to stretch it back out.
//
// This runs every frame rather than once, because it has to. The frontend's
// apply_graphics_config() rebuilds the graphics configuration from a
// default-constructed GraphicsConfig and assigns only the fields its own tab
// knows about, so pfm_option -- which the tab does not list -- is reset to its
// default every time any setting is applied. Pushing this once would survive
// until the player next touched the menu.
//
// The menu keeps showing what the player chose. It renders from the frontend's
// own option store, and apply_graphics_config only ever pushes from there into
// ultramodern; nothing reads back. So rewriting ultramodern's copy is invisible
// to the tab, which is what makes repurposing the setting possible without
// forking the frontend.
void update_widescreen_policy(int window_width, int window_height) {
    // OFF. The widening is stable now, and it still does not fix the culling.
    //
    // Four attempts, and the last one is the one that rules the whole approach
    // out: the game's frustum was genuinely widened for the whole of its frame
    // -- verified from both ends, the aspect held steady at the display's and
    // the matrix was narrowed back to the un-widened value at the handover --
    // and scenery still winks out at the sides. So Rayman 2's visibility test
    // reads neither the aspect passed to guPerspective nor the matrix built
    // from it, and nothing done at this function can reach it.
    //
    // Left in the tree because the measurements are worth more than the code:
    // RAYMAN2_WIDESCREEN=frustum re-enables it, RAYMAN2_NARROW=0 leaves the
    // wide matrix in place through to RT64, and RAYMAN2_DDPROBE=1 prints both
    // ends. Whatever finds the real culling will want those.
    static const bool enabled = []() {
        const char* mode = std::getenv("RAYMAN2_WIDESCREEN");
        return (mode != nullptr) && (std::strcmp(mode, "frustum") == 0);
    }();
    if (!enabled) {
        g_display_aspect.store(0.0f, std::memory_order_relaxed);
        return;
    }

    if (window_width <= 0 || window_height <= 0) {
        return;
    }

    namespace renderer = ultramodern::renderer;
    const renderer::GraphicsConfig& config = renderer::get_graphics_config();

    if (config.ar_option != renderer::AspectRatio::Expand) {
        g_display_aspect.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // Publish the DISPLAY aspect and let the hook divide, per call.
    //
    // Computing a single multiplier here was wrong, and measurably so. The game
    // does not use one aspect: the probe shows 1.3393 for its ordinary camera
    // and 1.9369 for the letterboxed cinematic one. A multiplier derived from
    // whichever value was seen last is right for that camera and wrong for the
    // other, and because the cinematic aspect is already wider than a 16:9
    // display, the ratio came out below one and was clamped away to nothing --
    // so the widening never happened at all during the very sequence being used
    // to test it.
    g_display_aspect.store(static_cast<float>(window_width) / static_cast<float>(window_height),
                           std::memory_order_relaxed);
}

} // namespace rayman2

// ---------------------------------------------------------------------------
// Undo the widening in the matrix, keep it everywhere else
// ---------------------------------------------------------------------------
//
// The widened aspect above is there so the game's own visibility test sees the
// frame the player is actually looking at. It must NOT reach the projection
// matrix, because RT64's Expand already widens that -- the two would multiply
// and the view would come out a third too wide.
//
// So the matrix is put back. guPerspective's only aspect-dependent term is
// [0][0], the horizontal scale, which is cot(fovy/2) / aspect: widening aspect
// by k divides it by k, and multiplying it back by k restores exactly the
// matrix an unmodified call would have produced. perspNorm is derived from near
// and far alone and is untouched.
//
// An N64 Mtx is 4x4 of s15.16 stored split: sixteen big-endian halfwords of
// integer parts, then sixteen of fractional parts. [0][0] is therefore the
// halfword at 0 and the halfword at 32.
//
// The saved pointer is what makes this work at the return: $a0 is long gone by
// then, so the start hook records it.

extern "C" void rayman2_record_projection_matrix(uint8_t* rdram, recomp_context* ctx) {
    const float k = g_perspective_k.exchange(1.0f, std::memory_order_relaxed);
    const uint32_t mtx = g_perspective_mtx.exchange(0, std::memory_order_relaxed);
    if (k == 1.0f || mtx == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pending_mutex);
    for (Pending& entry : g_pending) {
        if (entry.address == mtx) {
            entry.factor = k;   // rebuilt this frame; one narrowing still owed
            return;
        }
    }
    if (g_pending.size() < 16) {
        g_pending.push_back(Pending{ mtx, k });
    }
}

namespace rayman2 {

// Narrow every projection matrix widened since the last display list.
//
// This is the whole trick. The game and the renderer need different values out
// of the same sixteen numbers, and they can both have them because they read
// them at different times: the game culls while it is building its frame, and
// RT64 reads the matrix afterwards, when it parses the display list. So the
// matrix stays wide for the whole of the game's frame -- which is what its
// visibility test needs -- and is put back to what an unmodified guPerspective
// would have produced at the moment it is handed over, which is what RT64's own
// Expand needs.
//
// [0][0] is the only aspect-dependent term guPerspective writes. An N64 Mtx is
// 4x4 of s15.16 stored split, sixteen halfwords of integer parts followed by
// sixteen of fractional parts, so [0][0] is the halfword at 0 and the halfword
// at 32.
void narrow_pending_projections(uint8_t* rdram) {
    // RAYMAN2_NARROW=0 leaves the wide matrix in place all the way to RT64.
    // Worth having as a switch: if the winking-out is RT64 clipping against the
    // submitted matrix rather than the game culling at all, then narrowing it
    // here is what reintroduces the boundary, and leaving it wide removes it.
    static const bool narrowing = []() {
        const char* env = std::getenv("RAYMAN2_NARROW");
        return (env == nullptr) || (std::strcmp(env, "0") != 0);
    }();
    if (!narrowing) {
        std::lock_guard<std::mutex> lock(g_pending_mutex);
        g_pending.clear();
        return;
    }

    {
        static const bool on = std::getenv("RAYMAN2_DDPROBE") != nullptr;
        static int remaining = 4;
        if (on && remaining-- > 0) {
            std::lock_guard<std::mutex> lock(g_pending_mutex);
            std::fprintf(stderr, "[rayman2] send_dl: rdram=%p pending=%zu\n",
                         static_cast<const void*>(rdram), g_pending.size());
        }
    }

    if (rdram == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pending_mutex);
    for (const Pending& entry : g_pending) {
        const int64_t base = static_cast<int32_t>(entry.address);

        const int32_t integer = static_cast<int16_t>(MEM_H(0, base));
        const uint32_t fraction = static_cast<uint16_t>(MEM_H(32, base));
        const int32_t raw = (integer << 16) | static_cast<int32_t>(fraction);

        const double narrowed = (static_cast<double>(raw) / 65536.0) * static_cast<double>(entry.factor);
        if (!(narrowed > -32768.0 && narrowed < 32768.0)) {
            continue;
        }

        const int32_t out = static_cast<int32_t>(narrowed * 65536.0);
        MEM_H(0, base) = static_cast<int16_t>(out >> 16);
        MEM_H(32, base) = static_cast<int16_t>(out & 0xFFFF);

        static const bool on = std::getenv("RAYMAN2_DDPROBE") != nullptr;
        static int remaining = 6;
        if (on && remaining-- > 0) {
            std::fprintf(stderr, "[rayman2] narrow 0x%08X [0][0] %.5f -> %.5f (k=%.4f)\n",
                         entry.address, static_cast<double>(raw) / 65536.0, narrowed,
                         static_cast<double>(entry.factor));
        }
    }
    g_pending.clear();
}

} // namespace rayman2
