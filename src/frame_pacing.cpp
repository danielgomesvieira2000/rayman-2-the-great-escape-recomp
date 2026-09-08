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

namespace rayman2 {
    // src/demo_scan.cpp -- the attract-mode flag, and where it came from.
    bool attract_mode_active(const uint8_t* rdram);
    // src/main.cpp
    uint8_t* rdram_base();
}

namespace {

using clock_type = std::chrono::high_resolution_clock;

// How many VI fields each game frame is allowed to occupy. OFF by default.
//
// This started as a fix for docs/issues/004 and shipped disabled, because the
// diagnosis behind it was too broad and playtesting said so.
//
// The reasoning was: the game's loop blocks until the RDP reports finished,
// this runtime reports finished instantly, so the loop is paced by nothing and
// the whole game runs at twice its intended speed. The first half of that is
// right. The conclusion is not. Play says gameplay runs at the correct speed
// uncapped and only the attract-mode demos are fast, which fits a game whose
// physics advance on elapsed time -- and so self-correct at any frame rate --
// while demo playback is frame-indexed, one recorded input per frame, and
// therefore doubles when the frame rate does.
//
// So a global cap fixes a cosmetic defect in attract mode and costs the rest of
// the game half its frames, which is a bad trade and was rejected as one. It is
// kept, off, behind RAYMAN2_FRAMECAP, because the machinery is correct and the
// measurement is worth repeating when the demo is worth fixing properly -- see
// docs/issues/004 for what that needs.
//
// Expressed in FIELDS rather than in a frame rate, because fields are the only
// thing the game's frame can actually be aligned to and a rate that is not a
// divisor of 60 cannot be delivered evenly. RAYMAN2_FRAMECAP is still given in
// frames per second, because that is what a person means, and is converted here
// -- with the achieved rate reported, so asking for 45 and being given 30 is
// visible rather than mysterious.
int fields_per_frame() {
    static const int fields = []() {
        int value = 0;   // OFF. See the block comment above: the gate does not work.
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

// RAYMAN2_PACEPROBE=1 also reports the PRESENTATION interval.
//
// The pacing probe above measures when the game was told its frame was
// finished. That is not what the eye sees. What the eye sees is when a frame
// reaches the screen, and the two can come apart badly -- so a run where every
// completion is exactly two fields apart can still judder, and did.
//
// Reported side by side with the pacing so the difference is visible in one
// line rather than inferred across two runs.
void probe_presented(clock_type::duration delivered, clock_type::duration field) {
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
    std::fprintf(stderr, "[rayman2] present:%s   spread %.2f-%.2f fields\n",
                 used > 0 ? line : " (none)", worst_low, worst_high);
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
// Called from the renderer's draw hook, once per frame that reaches the screen.
void note_presented() {
    static const bool on = std::getenv("RAYMAN2_PACEPROBE") != nullptr;
    if (!on) {
        return;
    }
    const uint32_t speed = ultramodern::get_speed_multiplier();
    const auto field = std::chrono::duration_cast<clock_type::duration>(
        std::chrono::nanoseconds(std::chrono::seconds(1)) / (60 * (speed == 0 ? 1 : speed)));

    static clock_type::time_point previous{};
    const clock_type::time_point now = clock_type::now();
    if (previous.time_since_epoch().count() != 0) {
        probe_presented(now - previous, field);
    }
    previous = now;
}

// How far before the field boundary the completion lands. See pace_deadline.
//
// RAYMAN2_PACEMARGIN=<milliseconds> so the right value can be found by
// measuring rather than by choosing, which is what it took.
clock_type::duration pace_margin() {
    static const clock_type::duration margin = []() {
        int ms = 6;
        if (const char* env = std::getenv("RAYMAN2_PACEMARGIN")) {
            const long parsed = std::strtol(env, nullptr, 10);
            ms = static_cast<int>(parsed < 0 ? 0 : (parsed > 15 ? 15 : parsed));
            std::fprintf(stderr, "[rayman2] RAYMAN2_PACEMARGIN: completing %d ms before the field boundary\n", ms);
        }
        return std::chrono::duration_cast<clock_type::duration>(std::chrono::milliseconds(ms));
    }();
    return margin;
}

// When the RDP should appear to have finished this frame.
//
// Installed as ultramodern's dp_completion_pacer and called on the task thread
// immediately after the display list is handed to the renderer. It RETURNS a
// deadline; it does not wait for one.
//
// That distinction is the whole of the second fix. The first version slept
// here, inside send_dl, and the frame rate came out right while the picture
// juddered -- reported from play as "incredible judder". The task thread does
// not only carry display lists: it also carries the ScreenUpdateActions that
// decide which buffer the video interrupt presents. Sleeping on it holds those
// behind the wait by an amount that varies with when the game happened to swap,
// so the frames arrived evenly and reached the screen unevenly. Measured, the
// game's own completions were spaced 1.87 to 2.09 fields apart while what
// actually reached the screen ranged 1.64 to 2.33 -- and at thirty frames a
// second, where every frame has to be held for exactly two fields, that is
// visible.
//
// Returning a deadline lets ultramodern hold the completion while it goes on
// draining the queue, so a swap is applied the moment it is posted.
std::chrono::high_resolution_clock::time_point pace_deadline() {
    const int fields = fields_per_frame();

    // Intended to be only while an attract-mode demo is playing.
    //
    // It is not. attract_mode_active is true during gameplay as well -- see the
    // note on it -- so this gate does not gate. It is left here, unreached
    // because the cap defaults off, so that the next attempt has the shape to
    // fill in rather than to rebuild. docs/issues/004.
    const bool attract = rayman2::attract_mode_active(rayman2::rdram_base());
    {
        static bool engaged = false;
        if (attract != engaged) {
            engaged = attract;
            std::fprintf(stderr, "[rayman2] attract-mode demo %s: frame cap %s\n",
                         attract ? "started" : "ended",
                         attract ? "engaged" : "released");
        }
    }
    if (!attract) {
        return clock_type::now();   // due immediately: uncapped, as if no pacer
    }

    const uint32_t speed = ultramodern::get_speed_multiplier();
    const auto origin = ultramodern::get_start();
    const auto field = std::chrono::duration_cast<clock_type::duration>(
        std::chrono::nanoseconds(std::chrono::seconds(1)) / (60 * (speed == 0 ? 1 : speed)));

    const clock_type::time_point now = clock_type::now();
    if (fields <= 0 || field.count() <= 0) {
        return now;   // uncapped: due immediately, exactly as before
    }

    // The VI thread's own grid, not a schedule of our own. events.cpp puts
    // field k at get_start() + k/(60 * speed); a deadline anywhere else sits at
    // an arbitrary phase against the boundaries the picture is actually made
    // of, and frames land on either side of one at random.
    const long long now_field = (now <= origin) ? 0 : (now - origin) / field;

    static long long last_field = -1;
    long long target = (last_field < 0) ? now_field : last_field + fields;

    // Later than its deadline: the game was slower than the cap this frame, or
    // is drawing rarely because nothing is moving. Resync rather than carry the
    // shortfall, which would be repaid as a burst faster than the cap.
    if (target < now_field) {
        target = now_field;
    }
    last_field = target;

    // Land the completion a little BEFORE the boundary, not on it.
    //
    // The game does not present the frame; it is told the RDP is finished, then
    // does its own work and swaps, and the video interrupt shows whatever has
    // been swapped in by the time it comes round. Completing exactly on a
    // boundary is therefore the worst possible phase: the swap lands at the
    // boundary plus however long the game's frame-end work took that frame, and
    // that straddles the next boundary, so frames reach the screen one field
    // apart and then three. Measured as present: 1f:35 2f:80 3f:35.
    //
    // A margin gives the game room to finish and swap before the boundary it is
    // aiming at, so the same field shows it every time. It costs nothing: the
    // RDP simply appears to have taken slightly less than the whole budget.
    const clock_type::time_point deadline = origin + field * target - pace_margin();

    static clock_type::time_point previous{};
    if (previous.time_since_epoch().count() != 0) {
        probe(deadline - previous, field);
    }
    previous = deadline;

    return deadline;
}

// Install it, only if there is anything to do.
//
// With no pacer installed ultramodern completes the RDP exactly as it always
// did, so "off" costs not even an indirect call per frame.
void install_frame_pacing() {
    if (fields_per_frame() <= 0) {
        return;   // RAYMAN2_FRAMECAP=0: never pace, not even during a demo
    }
    ultramodern::set_dp_completion_pacer(pace_deadline);
}

} // namespace rayman2
