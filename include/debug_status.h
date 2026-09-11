/* What each of the port's debug facilities is doing, for something else to show.
 *
 * The port has accumulated a lot of instrumentation -- the draw distance and
 * widescreen arithmetic, the frame cap, the memory search, the attract-mode
 * scan, the capture key, the Controller Pak -- and every bit of it reports
 * itself the same way: a line printed to the log when it happens. That is the
 * right medium for an event and the wrong one for a state. "Is the memory
 * search armed", "is the camera being widened for culling right now",
 * "how many display lists has the game submitted" are questions about NOW, and
 * answering them from a scrolling log means reading backwards for the last line
 * that mentioned it and hoping nothing has changed since.
 *
 * So each facility also answers a struct, and src/debug_menu.cpp draws them all
 * in one window inside RT64's developer UI (F1). One struct per facility, each
 * filled by the file that owns the state, so a reader can go from a field on
 * screen to the code that decides it in one step.
 *
 * THESE ARE READ-ONLY, AND DELIBERATELY SO. Nothing here can change what the
 * port is doing. A debug menu that can also set things is a better tool and a
 * worse instrument: the first question about any odd behaviour becomes "did I
 * touch something in the menu", and the menu itself joins the list of suspects.
 * The controls come later, once the readouts have shown which of them are worth
 * having -- see docs/DEBUG-MENU.md.
 *
 * Every accessor here must be safe to call from the renderer's UI thread while
 * the game runs, which is why the values are copied out of atomics into a plain
 * struct rather than handing out pointers to live state.
 */

#ifndef RAYMAN2_DEBUG_STATUS_H
#define RAYMAN2_DEBUG_STATUS_H

#include <cstdint>
#include <filesystem>

namespace rayman2 {

/* src/demo_scan.cpp. The display-list counter is not really the scan's -- it is
 * fed by both renderers' send_dl and is the port's best measure of the game's
 * own frame rate -- but it lives there because that is what the scan labels its
 * samples with. */
struct DemoScanStatus {
    uint64_t display_lists = 0;   /* since launch, from both renderers */
    bool enabled = false;         /* RAYMAN2_DEMOSCAN */
    bool have_candidates = false;
    size_t candidates = 0;
    int idle_samples = 0;
    int busy_samples = 0;
};
DemoScanStatus demo_scan_status();

/* src/draw_distance.cpp -- both of the things that file does. The far plane is
 * the draw distance proper; the rest is the widescreen edge culling fix
 * (docs/issues/001), which widens the camera's field of view so that the game
 * culls against what a widescreen player can see, and then undoes the widening
 * in the finished matrix so the framing is unchanged.
 *
 * The three aspects are reported separately because they are three different
 * things that all read as "about 1.33" until the window is widened, and the
 * first version of this readout ran them together and said something false as a
 * result. None of them is "the aspect the port supplies": that write was tried
 * four times and abandoned, because it never reaches the game's visibility test
 * -- see the disabled block in src/draw_distance.cpp. `fov_widening` is what is
 * actually applied, and is the field to look at to answer "is the fix doing
 * anything right now". */
struct DrawDistanceStatus {
    float scale = 1.0f;             /* the far plane multiplier in force */
    bool scale_from_env = false;    /* RAYMAN2_DRAWDIST pinned it */
    float first_aspect = 0.0f;      /* the first the game ever asked for; the reference */
    float last_aspect = 0.0f;       /* the most recent guPerspective call's */
    float window_aspect = 0.0f;     /* the window's, as the port measured it */
    float fov_widening = 1.0f;      /* the factor applied to the camera; 1.0 is off */
    size_t pending_projections = 0; /* awaiting narrowing at the next send_dl */
};
DrawDistanceStatus draw_distance_status();

/* src/frame_pacing.cpp. The cap is off by default and the reason is a finding,
 * not an oversight -- see the block comment there and docs/issues/004. */
struct FramePacingStatus {
    int fields_per_frame = 0;     /* 0 is off; 2 is 30 fps, 1 is 60 */
    int margin_ms = 0;            /* RAYMAN2_PACEMARGIN */
    uint64_t presented = 0;       /* frames the renderer has put on screen */
};
FramePacingStatus frame_pacing_status();

/* Called once per presented frame by whichever renderer context is in use, so
 * that `presented` above means the same thing in both builds. */
void note_presented_frame();

/* src/memory_search.cpp. */
struct MemorySearchStatus {
    bool enabled = false;         /* RAYMAN2_MEMSEARCH */
    bool started = false;         /* F5 has been pressed */
    size_t candidates = 0;        /* offsets still in the running */
    size_t narrowings = 0;        /* F6/F7/F8 presses so far */
};
MemorySearchStatus memory_search_status();

namespace capture {
/* src/capture.cpp. */
struct Status {
    std::filesystem::path directory;   /* empty until set_output_directory */
    int taken = 0;                     /* this session */
};
Status status();
}  // namespace capture

}  // namespace rayman2

#endif /* RAYMAN2_DEBUG_STATUS_H */
