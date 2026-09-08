// Find the byte the game sets while an attract-mode demo is playing.
//
// WHY. docs/issues/004: the demos run at double speed and the rest of the game
// does not, so the frame cap that fixes them has to apply only while one is
// playing. The port has no way to know that. This finds it.
//
// HOW. The two states are separable from outside without knowing anything about
// the game: the title screen submits about two display lists a second because it
// is barely redrawing, and a demo submits sixty. So the port can label its own
// samples, and the flag is then whatever word is reliably one value while the
// label says title and another while it says demo.
//
// That is an ordinary cheat search with the labelling automated, and the
// automation is the point. The alternation happens on its own, every twenty
// seconds or so, for as long as the game is left alone; a person watching a
// memory viewer gets one transition per attempt and has to catch it.
//
// The intro cinematic is busy too and would be mislabelled. It is excluded by
// construction: nothing is sampled until the first idle period has been seen,
// and the intro is over by then.
//
// RAYMAN2_DEMOSCAN=1. Off otherwise, and it allocates nothing until asked.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// The N64's RDRAM, which is the whole of what the game can have put a flag in.
constexpr uint32_t kRamSize = 0x800000;
constexpr uint32_t kWords = kRamSize / 4;

// Rates that say which state the game is in, with a gap between them so that a
// transition is never labelled as either.
constexpr double kIdleBelow = 10.0;
constexpr double kBusyAbove = 40.0;

// How long a state must hold before it is sampled: long enough that a sample
// cannot land on a transition, short enough to fit inside a demo.
constexpr auto kSettle = std::chrono::seconds(3);

std::atomic<uint64_t> g_display_lists{0};

enum class State { Unknown, Idle, Busy };

struct Candidate {
    uint32_t offset;
    uint32_t idle_value;
    uint32_t busy_value;
};

std::vector<uint32_t> g_idle_snapshot;
std::vector<Candidate> g_candidates;
bool g_have_idle_snapshot = false;
bool g_have_candidates = false;
int g_idle_samples = 0;
int g_busy_samples = 0;

bool enabled() {
    static const bool on = std::getenv("RAYMAN2_DEMOSCAN") != nullptr;
    return on;
}

// An aligned word out of RDRAM, as the game would read it.
//
// RDRAM stores the byte for game address A at rdram[A ^ 3], so within an
// aligned four the bytes sit in reverse order. Copying those four into a
// little-endian host word therefore reassembles the big-endian value the game
// sees, exactly and without a swap -- which is worth stating because the first
// version of this "undid" a swap that had not happened and printed every value
// byte-reversed. 0x800CE258 came out as 0x58E20C80, which reads as noise rather
// than as the RAM pointer it is.
inline uint32_t raw_word(const uint8_t* rdram, uint32_t offset) {
    uint32_t value = 0;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}


void report(const char* what) {
    std::fprintf(stderr,
                 "[rayman2] demoscan: %s -- %zu candidates (title samples %d, demo samples %d)\n",
                 what, g_candidates.size(), g_idle_samples, g_busy_samples);

    if (g_candidates.empty() || g_candidates.size() > 24) {
        return;
    }
    for (const Candidate& c : g_candidates) {
        std::fprintf(stderr, "[rayman2]   0x%08X  title 0x%08X  demo 0x%08X\n",
                     0x80000000u + c.offset, c.idle_value, c.busy_value);
    }
}

// First idle sample: remember everything, because anything could be it.
void take_idle_snapshot(const uint8_t* rdram) {
    g_idle_snapshot.resize(kWords);
    for (uint32_t i = 0; i < kWords; i++) {
        g_idle_snapshot[i] = raw_word(rdram, i * 4);
    }
    g_have_idle_snapshot = true;
    g_idle_samples = 1;
    std::fprintf(stderr, "[rayman2] demoscan: title-screen snapshot taken (%u words)\n", kWords);
}

// First demo sample: everything that moved is a suspect.
void build_candidates(const uint8_t* rdram) {
    g_candidates.clear();
    for (uint32_t i = 0; i < kWords; i++) {
        const uint32_t now = raw_word(rdram, i * 4);
        if (now != g_idle_snapshot[i]) {
            g_candidates.push_back(Candidate{ i * 4, g_idle_snapshot[i], now });
        }
    }
    g_have_candidates = true;
    g_busy_samples = 1;
    g_idle_snapshot.clear();
    g_idle_snapshot.shrink_to_fit();
    report("first demo");
}

// Every sample after that only removes. A real flag holds the same value in
// every sample of the same state; anything that wavers is not one.
void filter(const uint8_t* rdram, State state) {
    const bool idle = (state == State::Idle);
    g_candidates.erase(
        std::remove_if(g_candidates.begin(), g_candidates.end(),
                       [&](const Candidate& c) {
                           const uint32_t now = raw_word(rdram, c.offset);
                           return now != (idle ? c.idle_value : c.busy_value);
                       }),
        g_candidates.end());
    if (idle) {
        g_idle_samples++;
        report("back at the title screen");
    }
    else {
        g_busy_samples++;
        report("demo again");
    }
}

// RAYMAN2_WATCH=0xADDR[,0xADDR...] -- print these words once a second.
//
// The scan can only label the two states it can tell apart from outside, so
// what it produces is a candidate that separates the title screen from a demo.
// That is not the same claim as separating a demo from GAMEPLAY, which is the
// one the frame cap actually depends on -- and getting it wrong caps gameplay,
// which is the outcome the whole exercise exists to avoid.
//
// So the candidates get watched through the states the scan never labelled: the
// intro cinematic, and play. The display-list rate is printed beside them so
// the state is legible without having to have been watching the screen.
void poll_watch(const uint8_t* rdram, double rate) {
    static std::vector<uint32_t> addresses = []() {
        std::vector<uint32_t> out;
        const char* env = std::getenv("RAYMAN2_WATCH");
        if (env == nullptr) {
            return out;
        }
        const char* p = env;
        while (*p != '\0' && out.size() < 8) {
            char* end = nullptr;
            const unsigned long value = std::strtoul(p, &end, 0);
            if (end == p) {
                break;
            }
            out.push_back(static_cast<uint32_t>(value));
            p = (*end == ',') ? end + 1 : end;
        }
        return out;
    }();
    if (addresses.empty()) {
        return;
    }

    char line[256];
    int used = 0;
    for (uint32_t address : addresses) {
        const uint32_t offset = address - 0x80000000u;
        if (offset >= kRamSize) {
            continue;
        }
        used += std::snprintf(line + used, sizeof(line) - used, " %08X", raw_word(rdram, offset));
    }
    std::fprintf(stderr, "[rayman2] watch:%s   (%.0f lists/s)\n", line, rate);
}

} // namespace

namespace rayman2 {

// Is the game playing an attract-mode sequence right now?
//
// THE SIGNATURE, and how it was found. src/demo_scan.cpp's own search, run
// unattended over five title-to-demo cycles: 2,097,152 words narrowed to three
// adjacent ones that hold one pair of values whenever the title screen is up
// and another whenever a demo is running.
//
//     0x800E4B18   title 0x00000003   attract 0x800CE258
//     0x800E4B1C   title 0x00000001   attract 0x00000005
//     0x800E4B20   title 0x00000000   attract 0x00000003
//
// The first is a RAM pointer while a demo plays and a small integer otherwise,
// which is what a pointer to the recorded input stream would look like. The
// other two read as a mode enum. They are adjacent, so this is one small
// structure rather than three coincidences.
//
// ALL THREE are required to match. Any one of them alone is a plausible flag
// and a plausible coincidence; three adjacent words agreeing is neither. The
// cost of a false positive here is capping the frame rate during play, which is
// the exact outcome this whole mechanism exists to avoid, so the test is made
// as specific as the evidence allows rather than as cheap as possible.
//
// WHAT THIS IS NOT VERIFIED AGAINST. The search could only label the two states
// the port can tell apart from outside -- the title screen and a demo -- so the
// signature is known to separate those. Gameplay was never sampled: a scripted
// run has no controller and cannot reach it. install_frame_pacing logs every
// time the cap engages, which is what makes the remaining claim checkable by
// somebody with a controller: if a line appears while you are playing, this is
// wrong.
bool attract_mode_active(const uint8_t* rdram) {
    if (rdram == nullptr) {
        return false;
    }
    return raw_word(rdram, 0x0E4B18) == 0x800CE258u
        && raw_word(rdram, 0x0E4B1C) == 0x00000005u
        && raw_word(rdram, 0x0E4B20) == 0x00000003u;
}

// Called from both renderers' send_dl, once per display list the game submits.
void note_display_list() {
    g_display_lists.fetch_add(1, std::memory_order_relaxed);
}

// Called once per frame from the thread that pumps events, which is where a
// valid rdram pointer and a steady tick are both available.
void demo_scan_poll(const uint8_t* rdram) {
    static const bool watching = std::getenv("RAYMAN2_WATCH") != nullptr;
    if ((!enabled() && !watching) || rdram == nullptr) {
        return;
    }

    using clock = std::chrono::steady_clock;
    static clock::time_point window_start = clock::now();
    static uint64_t window_base = 0;
    static State state = State::Unknown;
    static clock::time_point state_since = clock::now();
    static bool sampled_this_state = false;

    const clock::time_point now = clock::now();
    const auto elapsed = now - window_start;
    if (elapsed < std::chrono::milliseconds(500)) {
        return;
    }

    const uint64_t lists = g_display_lists.load(std::memory_order_relaxed);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const double rate = static_cast<double>(lists - window_base) * 1000.0 / static_cast<double>(ms);
    window_start = now;
    window_base = lists;

    poll_watch(rdram, rate);

    const State observed = (rate < kIdleBelow) ? State::Idle
                         : (rate > kBusyAbove) ? State::Busy
                                               : State::Unknown;

    if (observed == State::Unknown) {
        return;   // mid-transition; label nothing
    }
    if (observed != state) {
        state = observed;
        state_since = now;
        sampled_this_state = false;
        return;
    }
    if (sampled_this_state || (now - state_since) < kSettle) {
        return;
    }
    sampled_this_state = true;

    // Nothing is sampled until the title screen has been seen once, which is
    // what keeps the intro cinematic out of the "demo" label.
    if (!g_have_idle_snapshot && !g_have_candidates) {
        if (state == State::Idle) {
            take_idle_snapshot(rdram);
        }
        return;
    }
    if (!g_have_candidates) {
        if (state == State::Busy) {
            build_candidates(rdram);
        }
        return;
    }
    filter(rdram, state);
}

} // namespace rayman2
