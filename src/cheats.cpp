// The Cheats tab, and the cheats it turns on.
//
// A cheat here is a small, repeated write into the game's own memory rather
// than a patch to its code. That is the cheapest thing that works and the
// easiest to reason about: nothing is recompiled, the game is unmodified, and
// turning a cheat off stops the writes and leaves the game exactly as it was.
// It also means every cheat needs an ADDRESS, and finding one is the expensive
// part -- see src/memory_search.cpp, which is the tool for that.
//
// The tab is the port's own, created through recompui::config::create_config_tab
// rather than one of the prefab tabs, and saved to cheats.json beside the rest
// of the configuration. Values are read once a frame from the thread that pumps
// events and published as atomics, which is the same shape src/main.cpp uses for
// the sound volume and src/draw_distance.cpp for the graphics settings: the
// config store belongs to the frontend and is not documented as safe to read
// from anywhere else.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>

#ifdef RAYMAN2_ENABLE_FRONTEND
#include "recompui/recompui.h"
#include "recompui/config.h"
#endif

namespace {

// Where Rayman's health lives. NOT YET KNOWN.
//
// Zero means "no address", and every health cheat is inert until there is one.
// It is deliberately not a guess: writing a plausible-looking address every
// frame would corrupt whatever actually lives there, and the failure would look
// like anything at all.
//
// RAYMAN2_HEALTH_ADDR=0x8xxxxxxx supplies one without a rebuild, which is how a
// candidate from src/memory_search.cpp gets confirmed before it is baked in.
constexpr uint32_t kHealthAddressUnknown = 0;

std::atomic<bool> g_infinite_health{false};
std::atomic<uint32_t> g_health_address{kHealthAddressUnknown};
std::atomic<int32_t> g_health_value{-1};

const char* kTabId = "cheats";
const char* kInfiniteHealth = "infinite_health";
const char* kMemorySearch = "memory_search";

// How many bytes the cheat writes. ONE by default.
//
// This matters more than it looks, and the first version of this file got it
// wrong by always writing four. An N64 GameShark code of the form
//
//     80XXXXXX 00YY
//
// is "write the byte YY at RDRAM offset XXXXXX" -- the overwhelmingly common
// shape for a value like health -- and writing four bytes at that address
// instead would hold the intended byte AND flatten the three beside it, which
// belong to something else entirely. 81XXXXXX is the two-byte form.
//
// RAYMAN2_HEALTH_WIDTH=1, 2 or 4.
int configured_health_width() {
    static const int width = []() -> int {
        if (const char* env = std::getenv("RAYMAN2_HEALTH_WIDTH")) {
            const long parsed = std::strtol(env, nullptr, 0);
            if (parsed == 1 || parsed == 2 || parsed == 4) {
                return static_cast<int>(parsed);
            }
            std::fprintf(stderr, "[rayman2] RAYMAN2_HEALTH_WIDTH=%s is not 1, 2 or 4;"
                                 " using 1\n", env);
        }
        return 1;
    }();
    return width;
}

uint32_t configured_health_address() {
    static const uint32_t address = []() -> uint32_t {
        if (const char* env = std::getenv("RAYMAN2_HEALTH_ADDR")) {
            const unsigned long parsed = std::strtoul(env, nullptr, 0);
            const uint32_t value = static_cast<uint32_t>(parsed);
            // Only a KSEG0 address inside the 8 MB an N64 has is usable; a typo
            // should read as "no address" rather than as a write somewhere odd.
            if (value >= 0x80000000u && value < 0x80800000u) {
                std::fprintf(stderr, "[rayman2] RAYMAN2_HEALTH_ADDR: health at 0x%08X,"
                                     " %d byte(s)\n", value, configured_health_width());
                return value;
            }
            std::fprintf(stderr, "[rayman2] RAYMAN2_HEALTH_ADDR=%s is not a usable RDRAM address;"
                                 " ignoring it\n", env);
        }
        return kHealthAddressUnknown;
    }();
    return address;
}

// The value to hold health at. Whatever it reads at full health, which is not
// known either until the address is; RAYMAN2_HEALTH_VALUE supplies it for
// testing, and -1 means "whatever the highest value seen so far was".
int32_t configured_health_value() {
    static const int32_t value = []() -> int32_t {
        if (const char* env = std::getenv("RAYMAN2_HEALTH_VALUE")) {
            return static_cast<int32_t>(std::strtol(env, nullptr, 0));
        }
        return -1;
    }();
    return value;
}

// RDRAM, one byte at a time, at the address the game would use.
//
// The byte for game address A lives at rdram[(A - 0x80000000) ^ 3]. That XOR is
// not optional for byte and halfword access -- it is what makes a byte written
// here the byte the game reads -- and it was missing from the first version of
// this file, which only ever touched aligned words, where the reversal within
// each four happens to cancel out.
inline uint8_t read_byte(const uint8_t* rdram, uint32_t address) {
    return rdram[(address - 0x80000000u) ^ 3u];
}

inline void write_byte(uint8_t* rdram, uint32_t address, uint8_t value) {
    rdram[(address - 0x80000000u) ^ 3u] = value;
}

// Big-endian, like the console: the first byte is the most significant.
uint32_t read_value(const uint8_t* rdram, uint32_t address, int width) {
    uint32_t value = 0;
    for (int i = 0; i < width; i++) {
        value = (value << 8) | read_byte(rdram, address + static_cast<uint32_t>(i));
    }
    return value;
}

void write_value(uint8_t* rdram, uint32_t address, int width, uint32_t value) {
    for (int i = 0; i < width; i++) {
        const int shift = 8 * (width - 1 - i);
        write_byte(rdram, address + static_cast<uint32_t>(i),
                   static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

} // namespace

namespace rayman2 { void memory_search_set_enabled(bool on); }

namespace rayman2::cheats {

#ifdef RAYMAN2_ENABLE_FRONTEND
// Create the tab. Must be called before recompui::config::finalize(), like the
// prefab tabs beside it in src/frontend.cpp.
void create_tab() {
    recomp::config::Config& config =
        recompui::config::create_config_tab("Cheats", kTabId, false);

    config.add_bool_option(
        kMemorySearch,
        "Memory Search (F5-F8)",
        "Find an address by playing. F5 starts, F6 keeps what went down, F7 keeps "
        "what did not change, F8 keeps what went up. Counts and results are written "
        "to the session report in debug-report.",
        false);

    config.add_bool_option(
        kInfiniteHealth,
        "Infinite Health",
        "Rayman's health is held at full. Takes effect immediately, and turning "
        "it off leaves the game exactly as it was.",
        false);
}
#endif

// Read the tab's values. Called once a frame from the event pump.
//
// Every frame rather than once, for the same reason the sound volume is: the
// frontend rebuilds its configuration whenever any setting is applied, so a
// value read once would stop tracking the toggle the first time the player
// touched anything.
void refresh() {
#ifdef RAYMAN2_ENABLE_FRONTEND
    recomp::config::Config& config = recompui::config::get_config(kTabId);
    if (config.has_option(kInfiniteHealth)) {
        const auto value = config.get_option_value(kInfiniteHealth);
        if (const bool* on = std::get_if<bool>(&value)) {
            const bool was = g_infinite_health.exchange(*on, std::memory_order_relaxed);
            if (was != *on) {
                std::fprintf(stderr, "[rayman2] cheat: Infinite Health %s\n", *on ? "on" : "off");
                // Say why nothing happened, rather than leaving a toggle that
                // silently does nothing. The address is not known yet; see the
                // note on kHealthAddressUnknown and src/memory_search.cpp.
                if (*on && configured_health_address() == kHealthAddressUnknown) {
                    std::fprintf(stderr,
                        "[rayman2] cheat: ...but Rayman's health address is not known yet, so this"
                        " does nothing. Find it with RAYMAN2_MEMSEARCH=1 (F5 at full health, F6"
                        " after each hit, F7 after not being hit) and confirm it with"
                        " RAYMAN2_HEALTH_ADDR=0x........\n");
                }
            }
        }
    }
#endif
#ifdef RAYMAN2_ENABLE_FRONTEND
    {
        recomp::config::Config& config = recompui::config::get_config(kTabId);
        if (config.has_option(kMemorySearch)) {
            const auto value = config.get_option_value(kMemorySearch);
            if (const bool* on = std::get_if<bool>(&value)) {
                rayman2::memory_search_set_enabled(*on);
            }
        }
    }
#endif
    g_health_address.store(configured_health_address(), std::memory_order_relaxed);
}

// Apply whatever is on. Called once a frame with the game's RDRAM.
//
// Applied from the event pump rather than from a game thread, which is a
// deliberate limitation and worth naming: a cheat written once per presented
// frame can be overwritten by the game between writes, and a value the game
// recomputes every frame will flicker rather than stick. Health is not that
// kind of value -- it changes when something damages Rayman -- so a write per
// frame holds it. A cheat that needs to win a race with the game's own code
// would have to be a hook in the recompiled code instead.
void apply(uint8_t* rdram) {
    if (rdram == nullptr) {
        return;
    }
    const uint32_t address = g_health_address.load(std::memory_order_relaxed);
    if (address == kHealthAddressUnknown) {
        return;
    }

    // With an address configured, report what is there once a second.
    //
    // This is how a candidate from src/memory_search.cpp gets confirmed: point
    // RAYMAN2_HEALTH_ADDR at it, play, and watch whether the number falls when
    // Rayman is damaged and rises when he is healed. An address that does that
    // is health; one that does not is a coincidence that survived the search.
    // Setting the variable at all means somebody is doing exactly this, so it
    // reports without needing a second switch.
    {
        using clock = std::chrono::steady_clock;
        static clock::time_point last{};
        const clock::time_point now = clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            last = now;
            const int width = configured_health_width();
            const uint32_t value = read_value(rdram, address, width);
            std::fprintf(stderr, "[rayman2] cheat: [0x%08X] = %u (0x%0*X, %d byte(s))\n",
                         address, value, width * 2, value, width);
        }
    }

    if (g_infinite_health.load(std::memory_order_relaxed)) {
        int32_t target = configured_health_value();
        if (target < 0) {
            // No value given: hold at the highest seen, which is what full
            // health is as soon as the game has been at full health once.
            const int32_t current =
                static_cast<int32_t>(read_value(rdram, address, configured_health_width()));
            int32_t best = g_health_value.load(std::memory_order_relaxed);
            if (current > best) {
                g_health_value.store(current, std::memory_order_relaxed);
                best = current;
            }
            target = best;
        }
        if (target >= 0) {
            write_value(rdram, address, configured_health_width(),
                        static_cast<uint32_t>(target));
        }
    }
}

bool infinite_health_enabled() {
    return g_infinite_health.load(std::memory_order_relaxed);
}

uint32_t health_address() {
    return g_health_address.load(std::memory_order_relaxed);
}

} // namespace rayman2::cheats
