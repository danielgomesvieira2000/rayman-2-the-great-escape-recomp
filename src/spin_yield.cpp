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
// visible busy-wait for an invisible hang. One millisecond costs nothing and
// cannot deadlock.

#include <cstdint>

#include "ultramodern/ultramodern.hpp"

extern "C" void rayman2_yield_in_spin(uint8_t* rdram) {
    ultramodern::wait_for_external_message_timed(rdram, 1);
    ultramodern::check_running_queue(rdram);
}
