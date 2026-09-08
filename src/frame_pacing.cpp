// Give the game back the frame rate the RDP used to impose on it.
//
// THE PROBLEM. Rayman 2's frame loop is a strict handshake, described in the
// message-queue note in src/main.cpp: the producer takes the single free
// framebuffer slot, builds a display list and posts it, and blocks until the
// RDP completion returns that slot. So the loop advances exactly as fast as RDP
// completion comes back to it. The game is not paced by the video interrupt --
// it is paced by how long the RDP takes to draw, and on a console the RDP took
// longer than one field for a scene of this game.
//
// ultramodern completes it instantly. events.cpp calls dp_complete() on the
// line after send_dl() returns, the moment RT64 has accepted the display list,
// with no drawing time modelled at all. On a modern GPU that is microseconds:
// the slot comes straight back, the producer never waits, and the loop runs at
// the full 60 Hz VI rate instead of the roughly 30 the hardware allowed.
//
// Double the update rate is double the simulation speed. It is visible in the
// attract-mode demos, which are recorded input sequences and therefore the one
// part of the game with a duration a player already knows. See
// docs/issues/004.
//
// THE FIX, AND WHAT IT IS NOT. This paces the frame here, at the end of
// send_dl, which is the last thing that runs before dp_complete(). The port
// does not have to touch the runtime to do it, and delaying here is faithful to
// the mechanism: RT64 is handed the work immediately and only the completion
// signal waits, which is what an RDP that is still busy looks like from the
// game's side.
//
// It is a CAP, not a model of the RDP. A real RDP took as long as the scene
// needed, and no cap can reproduce that: a heavy scene on a console ran slower
// than this and a trivial one ran faster. What the cap fixes is the thing that
// is actually wrong -- a simulation advancing at twice its intended rate -- and
// it does so by limiting the rate rather than by pretending to know a duration.
// A scene that would have dropped below the cap on hardware still runs at the
// cap here, which is a small residual inaccuracy in the direction of running
// too fast, and enormously smaller than the factor of two it replaces.
//
// It only ever limits. The title screen submits about 2.3 display lists a
// second because the game is not redrawing, and nothing here speeds that up or
// slows it down; a frame that arrives later than its deadline resets the
// schedule rather than accumulating a debt to be paid back in a burst.
//
// WHY NOT A TIMER IN THE GAME'S OWN LOOP. Because the loop does not have one.
// It waits on the RDP, and the RDP is the port's to time. Anything else would
// be second-guessing the game's pacing from outside it.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "ultramodern/ultramodern.hpp"

namespace {

using clock_type = std::chrono::high_resolution_clock;

// How many VI fields each game frame is allowed to occupy.
//
// Two, because the defect is that the game runs at exactly twice its intended
// speed against a 60 Hz field rate, so one frame every two fields is 30 a
// second.
//
// Expressed in FIELDS rather than in a frame rate, because fields are the only
// thing the game's frame can actually be aligned to and a rate that is not a
// divisor of 60 cannot be delivered evenly. RAYMAN2_FRAMECAP is still given in
// frames per second, because that is what a person means, and is converted here
// -- with the achieved rate reported, so asking for 45 and being given 30 is
// visible rather than mysterious.
int fields_per_frame() {
    static const int fields = []() {
        int value = 2;
        if (const char* env = std::getenv("RAYMAN2_FRAMECAP")) {
            const long asked = std::strtol(env, nullptr, 10);
            if (asked <= 0) {
                std::fprintf(stderr, "[rayman2] RAYMAN2_FRAMECAP=0: no frame cap;"
                                     " the game will run at double speed\n");
                return 0;
            }
            value = static_cast<int>((60 + asked / 2) / asked);
            if (value < 1) {
                value = 1;
            }
            if (value > 60) {
                value = 60;
            }
            std::fprintf(stderr, "[rayman2] RAYMAN2_FRAMECAP=%ld: one frame every %d VI fields,"
                                 " so %d frames a second\n",
                         asked, value, 60 / value);
        }
        return value;
    }();
    return fields;
}

// RAYMAN2_PACEPROBE=1 -- what interval each frame was actually delivered at.
//
// A rate averaged over a second cannot see judder: thirty frames delivered as
// 2,2,2,2... and thirty delivered as 1,3,1,3... are both "30 a second", and only
// one of them looks right. What the eye is complaining about is the spread, so
// that is what this reports -- the measured gap between consecutive frames, in
// VI fields, bucketed.
//
// A cap that is working reads as one bucket. Anything else is the picture
// stuttering, however good the average looks.
void probe(clock_type::duration delivered, clock_type::duration field) {
    static const bool on = std::getenv("RAYMAN2_PACEPROBE") != nullptr;
    if (!on || field.count() <= 0) {
        return;
    }

    const double in_fields = static_cast<double>(delivered.count()) / static_cast<double>(field.count());
    const int bucket = static_cast<int>(in_fields + 0.5);

    static int counts[8] = {};
    static double worst_low = 1e9;
    static double worst_high = 0.0;
    static clock_type::time_point last_report = clock_type::now();

    counts[(bucket < 0) ? 0 : (bucket > 7 ? 7 : bucket)]++;
    if (in_fields < worst_low) worst_low = in_fields;
    if (in_fields > worst_high) worst_high = in_fields;

    const clock_type::time_point now = clock_type::now();
    if (now - last_report < std::chrono::seconds(5)) {
        return;
    }
    last_report = now;

    char line[256];
    int used = 0;
    for (int i = 0; i < 8; i++) {
        if (counts[i] != 0) {
            used += std::snprintf(line + used, sizeof(line) - used, " %df:%d", i, counts[i]);
        }
        counts[i] = 0;
    }
    std::fprintf(stderr, "[rayman2] pace:%s   spread %.2f-%.2f fields\n",
                 used > 0 ? line : " (no frames)", worst_low, worst_high);
    worst_low = 1e9;
    worst_high = 0.0;
}

} // namespace

namespace rayman2 {

// Called at the end of send_dl, on ultramodern's task thread.
//
// Sleeping here is not stealing time from anything: this thread exists to carry
// graphics tasks, audio tasks are dispatched from a different queue on a
// different thread (events.cpp enqueues an SpTaskAction only for M_GFXTASK), and
// the game itself is blocked waiting for exactly the completion being delayed.
void pace_frame() {
    const int fields = fields_per_frame();
    if (fields <= 0) {
        return;
    }

    // The VI thread's own grid, not a timer of our own.
    //
    // THIS IS THE WHOLE OF IT. The first version of this slept until
    // `previous + 1/30s`, which is a schedule with no relationship to the one
    // the video interrupt is running, and the result was judder: 1000/30 ms is
    // not exactly two fields, so the phase crept across a field boundary over
    // tens of seconds and frames were shown for one field or three instead of
    // two; and every time the game missed its deadline the phase was reset to
    // wherever that happened to land. The picture stuttered back and forth even
    // though the average rate was right.
    //
    // events.cpp puts field k at `get_start() + k/(60 * speed)`, so that is the
    // grid a frame has to land on. Snapping to it makes the interval exactly
    // `fields` fields every time, and the phase constant for the whole run.
    const uint32_t speed = ultramodern::get_speed_multiplier();
    const auto origin = ultramodern::get_start();
    const auto field = std::chrono::duration_cast<clock_type::duration>(
        std::chrono::nanoseconds(std::chrono::seconds(1)) / (60 * (speed == 0 ? 1 : speed)));

    const clock_type::time_point now = clock_type::now();
    const long long now_field = (now <= origin) ? 0 : (now - origin) / field;

    static long long last_field = -1;
    long long target = (last_field < 0) ? now_field : last_field + fields;

    // Later than its deadline: the game was slower than the cap this frame, or
    // is drawing rarely because nothing is moving. Resync to the grid rather
    // than carry the shortfall, which would be repaid as a burst of frames
    // faster than the cap -- the defect being fixed, in miniature.
    if (target < now_field) {
        target = now_field;
    }

    last_field = target;

    const clock_type::time_point deadline = origin + field * target;
    if (now < deadline) {
        ultramodern::sleep_until(deadline);
    }

    // Measured after the sleep, so it is what the game was actually given
    // rather than what it was meant to be given.
    static clock_type::time_point previous{};
    const clock_type::time_point delivered_at = clock_type::now();
    if (previous.time_since_epoch().count() != 0) {
        probe(delivered_at - previous, field);
    }
    previous = delivered_at;
}

} // namespace rayman2
