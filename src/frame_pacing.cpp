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

// The rate the game's loop is allowed to advance at, in display lists a second.
//
// Thirty, because the defect is that the game runs at exactly twice its
// intended speed against a 60 Hz field rate. That factor is what was reported
// from play and what the display-list rate measures: a pegged 60 a second in
// any scene with something happening in it.
//
// RAYMAN2_FRAMECAP=<n> overrides it and RAYMAN2_FRAMECAP=0 turns it off, which
// is how the two can be compared without a rebuild -- and how the measurement
// in docs/issues/004 can be repeated.
int target_rate() {
    static const int rate = []() {
        int value = 30;
        if (const char* env = std::getenv("RAYMAN2_FRAMECAP")) {
            const long parsed = std::strtol(env, nullptr, 10);
            // A cap above the field rate cannot bind, and a negative one is not
            // a rate. Both are clamped rather than refused, so a typo degrades
            // to "off" instead of to something unexplainable.
            value = static_cast<int>(parsed < 0 ? 0 : (parsed > 240 ? 240 : parsed));
            std::fprintf(stderr, "[rayman2] RAYMAN2_FRAMECAP: %s\n",
                         value == 0 ? "no frame cap; the game will run at double speed"
                                    : "frame cap overridden");
        }
        return value;
    }();
    return rate;
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
    const int rate = target_rate();
    if (rate <= 0) {
        return;
    }

    const auto period = std::chrono::nanoseconds(std::chrono::seconds(1)) / rate;
    const clock_type::time_point now = clock_type::now();

    static bool started = false;
    static clock_type::time_point next{};
    if (!started) {
        started = true;
        next = now + period;
        return;
    }

    if (now < next) {
        ultramodern::sleep_until(next);
        next += period;
    }
    else {
        // Later than its deadline: the game was slower than the cap this frame,
        // or is drawing rarely because nothing is moving. Either way the cap has
        // nothing to do, and carrying the shortfall forward would repay it as a
        // burst of frames faster than the cap -- which is the defect being
        // fixed, in miniature.
        next = now + period;
    }
}

} // namespace rayman2
