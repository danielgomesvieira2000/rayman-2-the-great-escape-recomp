// Find Rayman's health every session, instead of remembering where it was once.
//
// WHY. The first Infinite Health shipped a hardcoded 0x80231870, confirmed
// against the attract demo and broken everywhere else: 0x8023xxxx is heap, so
// that is where Rayman's object happened to land in that one sequence. In a real
// session the same address read 0x00000000 and 0xCCCCCCCC -- uninitialised fill,
// memory the game had never written. A hardcoded heap address is not an address,
// it is a coincidence with a number written on it.
//
// WHAT IS ACTUALLY FIXED. 0x800EF6D4 is in the game's static data and always
// holds health scaled by ten thirds -- 50.0 when health is 15, 43.333 when it is
// 13, at every sample across every run. That relationship is the handle: the
// mirror says what health IS, from an address that never moves, and the only
// question left is where the value it was derived from lives this time.
//
// So each session: read the mirror, work out what health must be, and find the
// word that holds exactly that and goes on holding exactly that as the mirror
// changes. Health moves during play, which is what makes this cheap -- a value
// that tracks a changing target across several samples is not a coincidence.
//
// The search runs until it has one address and then stops. It costs a scan of
// RDRAM per sample while it is narrowing, and nothing at all afterwards.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kRamSize = 0x800000;

// The static mirror, and the ratio it holds health at. Both established by
// watching all three candidates through a demo; see src/cheats.cpp.
constexpr uint32_t kMirrorAddress = 0x800EF6D4;
constexpr float kMirrorRatio = 10.0f / 3.0f;

// Full health, and the range a plausible health value lives in.
constexpr float kHealthFull = 15.0f;
constexpr float kHealthMax = 100.0f;

// Enough agreeing samples to believe an address, and few enough to be quick.
// Health changes every time Rayman is hit, so a candidate that tracks it three
// times running is tracking it.
constexpr int kSamplesToConfirm = 3;

std::vector<uint32_t> g_candidates;
int g_samples = 0;
std::atomic<uint32_t> g_found{0};
bool g_reported = false;

float read_float(const uint8_t* rdram, uint32_t offset) {
    uint32_t raw = 0;
    std::memcpy(&raw, rdram + offset, sizeof(raw));
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

// What health must be right now, from the mirror that never moves.
// Returns false when the mirror holds nothing usable -- before a level is
// loaded, or between them.
bool expected_health(const uint8_t* rdram, float& out) {
    const float mirror = read_float(rdram, kMirrorAddress - 0x80000000u);
    if (!(mirror > 0.0f) || mirror > kHealthMax * kMirrorRatio) {
        return false;
    }
    const float health = mirror / kMirrorRatio;
    // Health is a whole number of hit points stored as a float. Rounding here
    // is what makes an exact comparison possible below, and a value that is not
    // close to a whole number is not health.
    const float rounded = std::round(health);
    if (std::fabs(health - rounded) > 0.01f || rounded <= 0.0f || rounded > kHealthMax) {
        return false;
    }
    out = rounded;
    return true;
}

} // namespace

namespace rayman2 {

// The address health is at this session, or 0 while it is still being found.
uint32_t health_address_now() {
    return g_found.load(std::memory_order_relaxed);
}

// Called once a frame. Does nothing once health has been located.
void health_locator_poll(const uint8_t* rdram) {
    if (rdram == nullptr || g_found.load(std::memory_order_relaxed) != 0) {
        return;
    }

    // Sampled rather than run every frame: the point is to see the target
    // CHANGE, and consecutive frames rarely differ.
    using clock = std::chrono::steady_clock;
    static clock::time_point next{};
    const clock::time_point now = clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::milliseconds(500);

    float target = 0.0f;
    if (!expected_health(rdram, target)) {
        return;
    }

    uint32_t target_bits = 0;
    std::memcpy(&target_bits, &target, sizeof(target_bits));

    if (g_candidates.empty() && g_samples == 0) {
        // First pass: every word in RDRAM that holds exactly this value.
        for (uint32_t offset = 0; offset < kRamSize; offset += 4) {
            uint32_t raw = 0;
            std::memcpy(&raw, rdram + offset, sizeof(raw));
            if (raw == target_bits) {
                g_candidates.push_back(offset);
            }
        }
        g_samples = 1;
        std::fprintf(stderr, "[rayman2] health: %zu words hold %.0f; watching them\n",
                     g_candidates.size(), static_cast<double>(target));
        return;
    }

    // Every pass after that only removes. The mirror moves when Rayman is hit,
    // and a word that is still equal to it afterwards is still tracking it.
    const size_t before = g_candidates.size();
    g_candidates.erase(
        std::remove_if(g_candidates.begin(), g_candidates.end(),
                       [&](uint32_t offset) {
                           uint32_t raw = 0;
                           std::memcpy(&raw, rdram + offset, sizeof(raw));
                           return raw != target_bits;
                       }),
        g_candidates.end());

    // Only count a sample that could actually discriminate. While health is
    // unchanged every candidate agrees, and confirming on those would confirm
    // nothing.
    static float last_target = -1.0f;
    if (target != last_target) {
        last_target = target;
        g_samples++;
    }

    if (g_candidates.empty()) {
        // The value moved somewhere this search could not follow -- a level
        // change, or a respawn that reallocated the object. Start again.
        g_samples = 0;
        std::fprintf(stderr, "[rayman2] health: lost the trail, restarting the search\n");
        return;
    }

    if (before != g_candidates.size()) {
        std::fprintf(stderr, "[rayman2] health: %zu candidates after %d changes\n",
                     g_candidates.size(), g_samples);
    }

    if (g_samples >= kSamplesToConfirm && g_candidates.size() == 1) {
        const uint32_t address = 0x80000000u + g_candidates.front();
        g_found.store(address, std::memory_order_relaxed);
        if (!g_reported) {
            g_reported = true;
            std::fprintf(stderr, "[rayman2] health: found at 0x%08X (full health is %.0f)\n",
                         address, static_cast<double>(kHealthFull));
        }
    }
}

// Forget it, so the next level's allocation is found afresh.
void health_locator_reset() {
    g_found.store(0, std::memory_order_relaxed);
    g_candidates.clear();
    g_samples = 0;
    g_reported = false;
}

} // namespace rayman2
