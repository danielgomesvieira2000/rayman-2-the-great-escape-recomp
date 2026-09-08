/* One keypress that turns "that looked wrong" into a filed issue.
 *
 * A graphics bug is only as fixable as the evidence taken at the moment it was
 * on screen, and evidence reconstructed afterwards is the weakest kind: the
 * exact spot is gone, the settings have been fiddled with, and the description
 * has already collapsed into "the water looked odd". So F9 writes everything
 * worth having, at once, while the frame is still up:
 *
 *   - what the player sees, as a screenshot of the window;
 *   - what the game drew, as the raw N64 framebuffer out of RDRAM, which is a
 *     different picture whenever the renderer is scaling or widening;
 *   - every graphics setting in force, because "does it change with resolution,
 *     aspect or MSAA" is the first question and it should never have to be
 *     asked;
 *   - a pre-filled issue stub, so filing it is editing a file rather than
 *     starting one.
 *
 * It deliberately does NOT try to capture the display list. RT64 already has a
 * far better tool for that -- the frame inspector on F1, with Developer Mode on
 * -- which can pause the frame and walk the draw calls interactively. See
 * docs/DEBUG-REPORTS.md.
 */

#ifndef RAYMAN2_CAPTURE_H
#define RAYMAN2_CAPTURE_H

#include <cstdint>
#include <filesystem>

namespace rayman2::capture {

/* Where captures are written: a "captures" folder beside the session reports.
 * Call once, with the same directory the debug report chose, so that a player
 * who has found one has found the other. */
void set_output_directory(const std::filesystem::path& debug_report_dir);

/* Call once per frame from the thread that pumps SDL events. Edge-detects the
 * capture key (F9) and takes a capture when it goes down. Cheap enough to call
 * unconditionally; does nothing until set_output_directory has been called. */
void poll_hotkey(uint8_t* rdram);

/* Take a capture now. `reason` is written into the stub's title line -- "F9"
 * for the hotkey, or whatever else asked for it. */
void take(uint8_t* rdram, const char* reason);

} // namespace rayman2::capture

#endif /* RAYMAN2_CAPTURE_H */
