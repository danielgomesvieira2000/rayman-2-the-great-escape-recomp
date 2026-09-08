// Let a busy-waiting game thread give the scheduler a turn.
//
// THE PROBLEM. libultra's scheduler is preemptive: a counter interrupt fires
// regardless of what the running thread is doing, and a higher-priority thread
// that has just been made runnable takes over. Game code written against that
// is entitled to spin on a flag and expect somebody else to clear it.
//
// ultramodern's scheduler is not preemptive. It reschedules in exactly one
// place -- check_running_queue, called from osSendMesg, osRecvMesg and
// osJamMesg -- and it delivers messages posted from outside the game (VI
// retrace, SP and DP completion, PI and SI) in exactly one place too, at the
// top of those same calls. A thread that spins without calling any of them
// therefore blocks the entire game: no other thread runs, and no external
// event is ever delivered.
//
// Rayman 2 hits this at func_8008F344, which is the last thing its graphics
// initialisation does:
//
//     func_8008EF6C();
//     while (func_8008EF6C() != 0) { }      // reads one byte, calls nothing
//     func_8008F314();
//
// The byte is cleared by the worker thread at func_8008EF78, which is blocked
// in osRecvMesg waiting for a message it can only receive if somebody pumps the
// external queue. Measured, the spin turned 519 million times in fourteen
// seconds while the worker ran 21 times and then stopped -- a deadlock, and the
// reason the port reached its graphics init and never submitted a display list.
//
// THE FIX. A hook in the loop body calls this, which does what the counter
// interrupt would have done: deliver a pending external message and give a
// higher-priority runnable thread the CPU.
//
// The wait is bounded rather than indefinite. wait_for_external_message would
// be the exact analogue and is normally safe, because VI retrace alone supplies
// a message every frame -- but "normally" is doing real work in that sentence,
// and a spin that turns into a hard block if the supply ever dries up trades a
// visible busy-wait for an invisible hang.
//
// MEASURED, not reasoned. The obvious refinement is to poll with a zero timeout
// in waits that run once per frame, on the argument that a millisecond of
// latency is a large fraction of a sixteen-millisecond frame. Measured over
// five runs each, that is wrong: polling gave peak display-list rates of
// 34, 37, 45, 15 and 18 per second, and sleeping one millisecond gave
// 33, 36, 39, 50 and 40 -- better on the median and far steadier. A hot poll
// takes CPU away from the native renderer and audio threads, and losing that
// costs more than the latency saves. One millisecond it is.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "ultramodern/ultramodern.hpp"

namespace {

// RAYMAN2_YIELD_MS=<n> -- how long a spinning thread waits for an external
// message before giving the scheduler a turn. One millisecond by default,
// arrived at by measurement (see above).
//
// It is exposed as a switch for docs/issues/003. This value is the port's
// largest single influence on when the game's threads run relative to each
// other, and the intro's intermittent null call is a scheduling-shaped failure:
// same build, same machine, same inputs, different outcomes. If moving this
// moves the failure rate, the fault is a race and the search narrows to the
// threads involved; if it does not, the whole hypothesis is out and that is
// worth just as much.
//
// Clamped rather than trusted: 0 is a hot poll, which was measured to be worse
// than sleeping, and an unbounded value would turn a spin into a stall.
uint32_t yield_timeout_ms() {
    static const uint32_t ms = []() -> uint32_t {
        const char* value = std::getenv("RAYMAN2_YIELD_MS");
        if (value == nullptr) {
            return 1;
        }
        const long parsed = std::strtol(value, nullptr, 10);
        const uint32_t clamped = static_cast<uint32_t>(parsed < 0 ? 0 : (parsed > 100 ? 100 : parsed));
        std::fprintf(stderr, "[rayman2] RAYMAN2_YIELD_MS: spin yields wait %u ms (default 1)\n",
                     clamped);
        return clamped;
    }();
    return ms;
}

} // namespace

// Deliver one pending external event and let a higher-priority runnable thread
// take over -- what the counter interrupt would have done.
extern "C" void rayman2_yield_in_spin(uint8_t* rdram) {
    ultramodern::wait_for_external_message_timed(rdram, yield_timeout_ms());
    ultramodern::check_running_queue(rdram);
}
