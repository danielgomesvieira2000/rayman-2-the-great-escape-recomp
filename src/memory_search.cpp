// Find a value in the game's memory by playing, not by reading disassembly.
//
// WHY THIS EXISTS. A cheat needs an address, and this port has no symbols for
// the game's data. src/demo_scan.cpp can find an address on its own, but only
// when the two states it has to tell apart are distinguishable from outside --
// it labelled the title screen against a demo using the display-list rate.
// Rayman's health has no such signal: nothing visible from the port says
// "damaged just now". A person playing knows, so the labelling has to come from
// them, one keypress at a time.
//
// This is therefore the standard cheat search, with the port supplying the
// scanning and the player supplying the labels.
//
// HOW TO USE IT. Launch with RAYMAN2_MEMSEARCH=1, get into the game, then:
//
//   F5   start (or restart): remember every word in RDRAM
//   F6   keep only what DECREASED since the last snapshot, and re-snapshot
//   F7   keep only what is UNCHANGED since the last snapshot, and re-snapshot
//   F8   keep only what INCREASED since the last snapshot, and re-snapshot
//
// For health: stand somewhere safe at full health and press F5. Take a hit,
// press F6. Take another, press F6. Walk around without being hit, press F7.
// Repeat until the count is small enough to print, which is usually four or
// five presses. Anything that survives is a value that went down exactly when
// Rayman was damaged and held still exactly when he was not.
//
// F7 is the one that does the work. "It went down" is true of hundreds of
// thousands of words in a running game -- timers, positions, counters -- and
// "it went down when I was hit and did not move when I was not" is true of
// almost nothing else.
//
// The keys avoid F1 to F4, which are RT64's, and F9, which is the capture.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "SDL.h"

namespace {

constexpr uint32_t kRamSize = 0x800000;
constexpr uint32_t kWords = kRamSize / 4;

// Above this the list is counted rather than printed: a search still holding
// thousands of candidates has nothing a person can read in it.
constexpr size_t kPrintLimit = 32;

std::vector<uint32_t> g_snapshot;      // value at the last snapshot, per candidate
std::vector<uint32_t> g_candidates;    // offsets still in the running
bool g_started = false;

bool enabled() {
    static const bool on = std::getenv("RAYMAN2_MEMSEARCH") != nullptr;
    return on;
}

uint32_t read_word(const uint8_t* rdram, uint32_t offset) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        reinterpret_cast<uint8_t*>(&value)[i] = rdram[offset + i];
    }
    return value;
}

void report(const char* what) {
    std::fprintf(stderr, "[rayman2] memsearch: %s -- %zu candidates\n", what, g_candidates.size());
    if (g_candidates.empty() || g_candidates.size() > kPrintLimit) {
        return;
    }
    for (size_t i = 0; i < g_candidates.size(); i++) {
        std::fprintf(stderr, "[rayman2]   0x%08X = %d (0x%08X)\n",
                     0x80000000u + g_candidates[i],
                     static_cast<int32_t>(g_snapshot[i]), g_snapshot[i]);
    }
    std::fprintf(stderr, "[rayman2]   confirm one with RAYMAN2_HEALTH_ADDR=0x........\n");
}

void start(const uint8_t* rdram) {
    g_candidates.clear();
    g_snapshot.clear();
    g_candidates.reserve(kWords);
    g_snapshot.reserve(kWords);
    for (uint32_t i = 0; i < kWords; i++) {
        g_candidates.push_back(i * 4);
        g_snapshot.push_back(read_word(rdram, i * 4));
    }
    g_started = true;
    std::fprintf(stderr, "[rayman2] memsearch: started -- %zu words remembered."
                         " Now change the value and press F6 (went down),"
                         " F7 (unchanged) or F8 (went up).\n", g_candidates.size());
}

enum class Direction { Down, Same, Up };

void narrow(const uint8_t* rdram, Direction direction) {
    if (!g_started) {
        std::fprintf(stderr, "[rayman2] memsearch: press F5 first\n");
        return;
    }

    std::vector<uint32_t> kept_offsets;
    std::vector<uint32_t> kept_values;
    kept_offsets.reserve(g_candidates.size());
    kept_values.reserve(g_candidates.size());

    for (size_t i = 0; i < g_candidates.size(); i++) {
        const uint32_t now = read_word(rdram, g_candidates[i]);
        // Signed, because a health value that has gone below zero is still a
        // value that went down, and an unsigned compare would call that an
        // enormous increase.
        const int32_t before = static_cast<int32_t>(g_snapshot[i]);
        const int32_t after = static_cast<int32_t>(now);

        const bool keep = (direction == Direction::Down) ? (after < before)
                        : (direction == Direction::Same) ? (after == before)
                                                         : (after > before);
        if (keep) {
            kept_offsets.push_back(g_candidates[i]);
            kept_values.push_back(now);
        }
    }

    g_candidates.swap(kept_offsets);
    g_snapshot.swap(kept_values);
    report(direction == Direction::Down ? "went down"
         : direction == Direction::Same ? "unchanged"
                                        : "went up");
}

} // namespace

namespace rayman2 {

// Called once a frame from the thread that pumps events, which is where the
// keyboard state is valid and where a capture is already polled from.
void memory_search_poll(const uint8_t* rdram) {
    if (!enabled() || rdram == nullptr) {
        return;
    }

    int num_keys = 0;
    const Uint8* keys = SDL_GetKeyboardState(&num_keys);
    if (keys == nullptr || num_keys <= SDL_SCANCODE_F8) {
        return;
    }

    // Edge-detected, or a held key would run the scan every frame and empty the
    // list on the first one.
    struct Key { SDL_Scancode code; bool was_down; };
    static Key f5{ SDL_SCANCODE_F5, false };
    static Key f6{ SDL_SCANCODE_F6, false };
    static Key f7{ SDL_SCANCODE_F7, false };
    static Key f8{ SDL_SCANCODE_F8, false };

    const auto pressed = [&](Key& key) {
        const bool down = keys[key.code] != 0;
        const bool edge = down && !key.was_down;
        key.was_down = down;
        return edge;
    };

    if (pressed(f5)) {
        start(rdram);
    }
    if (pressed(f6)) {
        narrow(rdram, Direction::Down);
    }
    if (pressed(f7)) {
        narrow(rdram, Direction::Same);
    }
    if (pressed(f8)) {
        narrow(rdram, Direction::Up);
    }
}

} // namespace rayman2
