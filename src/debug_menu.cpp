// The port's debug menu. See include/debug_menu.h for what it is for.

#include "debug_menu.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <imgui.h>

#include <librecomp/game.hpp>
#include <ultramodern/config.hpp>
#include <ultramodern/renderer_context.hpp>

#include "capture.h"
#include "controller_pak.h"
#include "debug_report.h"
#include "debug_status.h"

// Set by this file, called by RT64 once per frame with an ImGui frame already
// open. tools/patch_rt64_debug_menu.py adds the declaration and the call.
extern "C" void (*RT64_PortDebugMenuHook)();

namespace rayman2::debug_menu {
namespace {

using clock_type = std::chrono::steady_clock;

bool g_enabled = false;
clock_type::time_point g_started;

// Rates, from the counters that only ever go up.
//
// The counters are totals since launch, and a total is the wrong thing to watch
// a frame rate with -- by the time it is large enough to read, a stall of a
// second has moved it by a percent. So each is differenced against its own value
// a second ago. Sampled on the UI thread, which runs only while the window is
// open, so the first reading after opening it is discarded rather than being a
// rate measured over however long the menu was shut.
struct Rate {
    uint64_t last_value = 0;
    clock_type::time_point last_time{};
    double per_second = 0.0;
    bool primed = false;

    void update(uint64_t value) {
        const clock_type::time_point now = clock_type::now();
        if (!primed) {
            last_value = value;
            last_time = now;
            primed = true;
            return;
        }
        const std::chrono::duration<double> elapsed = now - last_time;
        if (elapsed.count() < 0.5) {
            return;
        }
        per_second = double(value - last_value) / elapsed.count();
        last_value = value;
        last_time = now;
    }
};

// A label and a value on one row, so every section lines up with the others.
// ImGui's own two-column tables would do this, but a table per section puts a
// scroll region and a border around each and the window is a readout, not a
// grid of boxes.
constexpr float kLabelWidth = 190.0f;

void row(const char* label, const char* fmt, ...) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(kLabelWidth);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

void row_path(const char* label, const std::filesystem::path& path) {
    // A path is the one field long enough to push the window wide, so it wraps
    // onto as many lines as it needs rather than being cut off -- a truncated
    // path is the one thing here that is worse than useless, because it looks
    // like an answer.
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(kLabelWidth);
    if (path.empty()) {
        ImGui::TextDisabled("(none)");
        return;
    }
    const std::string text = path.string();
    ImGui::TextWrapped("%s", text.c_str());
}

const char* yes_no(bool value) {
    return value ? "yes" : "no";
}

// The graphics settings, named as the frontend names them. src/capture.cpp has
// the same four converters for the capture's settings block; they are duplicated
// rather than shared because the alternative is a header whose only purpose is
// to hold four switch statements, and each copy is next to the thing it labels.
const char* name_of(ultramodern::renderer::AspectRatio value) {
    switch (value) {
        case ultramodern::renderer::AspectRatio::Original: return "Original (4:3)";
        case ultramodern::renderer::AspectRatio::Expand:   return "Expand";
        default:                                           return "Manual";
    }
}

const char* name_of(ultramodern::renderer::HUDRatioMode value) {
    switch (value) {
        case ultramodern::renderer::HUDRatioMode::Original:  return "Original";
        case ultramodern::renderer::HUDRatioMode::Clamp16x9: return "Clamp 16:9";
        case ultramodern::renderer::HUDRatioMode::Full:      return "Full";
        default:                                             return "?";
    }
}

const char* name_of(ultramodern::renderer::Antialiasing value) {
    switch (value) {
        case ultramodern::renderer::Antialiasing::None:   return "off";
        case ultramodern::renderer::Antialiasing::MSAA2X: return "MSAA 2x";
        case ultramodern::renderer::Antialiasing::MSAA4X: return "MSAA 4x";
        case ultramodern::renderer::Antialiasing::MSAA8X: return "MSAA 8x";
        default:                                          return "?";
    }
}

const char* name_of(ultramodern::renderer::Resolution value) {
    switch (value) {
        case ultramodern::renderer::Resolution::Original:   return "Original (1x)";
        case ultramodern::renderer::Resolution::Original2x: return "2x native";
        case ultramodern::renderer::Resolution::Auto:       return "Auto (match window)";
        case ultramodern::renderer::Resolution::Native3x:   return "3x native";
        case ultramodern::renderer::Resolution::Native4x:   return "4x native";
        default:                                            return "?";
    }
}

const char* name_of(ultramodern::renderer::PresentFillMode value) {
    switch (value) {
        case ultramodern::renderer::PresentFillMode::Crop:      return "Crop";
        case ultramodern::renderer::PresentFillMode::Stretch:   return "Stretch";
        case ultramodern::renderer::PresentFillMode::Pillarbox: return "Pillarbox";
        default:                                                return "?";
    }
}

const char* name_of(ultramodern::renderer::RefreshRate value) {
    switch (value) {
        case ultramodern::renderer::RefreshRate::Original: return "Original";
        case ultramodern::renderer::RefreshRate::Display:  return "Display";
        default:                                           return "Manual";
    }
}

// Which build this is. The two differ in enough ways -- who owns the RT64
// application, whether there is a launcher, which renderer context file is
// compiled -- that a report or a screenshot of this window is ambiguous without
// it, and that ambiguity has cost time before (docs/issues/004 was measured on
// one build and fixed in the other).
constexpr const char* kBuild =
#ifdef RAYMAN2_ENABLE_FRONTEND
    "frontend";
#else
    "headless";
#endif

void draw_session() {
    const std::chrono::duration<double> up = clock_type::now() - g_started;
    const int seconds = static_cast<int>(up.count());
    row("build", "%s", kBuild);
    row("uptime", "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    row("errors / crashes", "%d / %d", rayman2::report::error_count(),
        rayman2::report::crash_count());
    row_path("config", recomp::get_config_path());
    row_path("session report", rayman2::report::path());
}

void draw_renderer() {
    const DemoScanStatus scan = demo_scan_status();
    const FramePacingStatus pacing = frame_pacing_status();

    static Rate lists;
    static Rate frames;
    lists.update(scan.display_lists);
    frames.update(pacing.presented);

    // Two different rates, and the difference between them is the interesting
    // part. Display lists are the GAME's frame rate -- one per frame it draws.
    // Presented frames are the RENDERER's. They are equal under the default
    // presentation and diverge the moment RT64 is interpolating or the game has
    // stalled while the window keeps painting.
    row("display lists", "%llu  (%.1f/s)",
        static_cast<unsigned long long>(scan.display_lists), lists.per_second);
    row("frames presented", "%llu  (%.1f/s)",
        static_cast<unsigned long long>(pacing.presented), frames.per_second);
    row("this menu", "%.1f fps", double(ImGui::GetIO().Framerate));

    if (pacing.fields_per_frame == 0) {
        row("frame cap", "off");
    }
    else {
        row("frame cap", "%d fields/frame (%.1f fps), %d ms margin",
            pacing.fields_per_frame, 60.0 / double(pacing.fields_per_frame),
            pacing.margin_ms);
    }
}

void draw_graphics() {
    const ultramodern::renderer::GraphicsConfig& gfx =
        ultramodern::renderer::get_graphics_config();
    row("aspect ratio", "%s", name_of(gfx.ar_option));
    row("HUD ratio", "%s", name_of(gfx.hr_option));
    row("antialiasing", "%s", name_of(gfx.msaa_option));
    row("refresh rate", "%s (%d Hz)", name_of(gfx.rr_option), gfx.rr_manual_value);
    row("internal resolution", "%s", name_of(gfx.res_option));
    // Reported as the raw option rather than as a multiplier, because it is not
    // always the multiplier RT64 ends up using -- the two were observed
    // disagreeing (config 0, RT64's own panel 1), and dressing the number up as
    // "0x" makes a discrepancy look like a broken setting.
    row("downsample option", "%d", gfx.ds_option);
    row("present fill", "%s", name_of(gfx.pfm_option));
    // Not a setting the player can reach: RecompFrontend registers Dev Mode
    // hidden, and the port forces it on anyway so that F1 works. Shown because
    // this window existing at all is the visible consequence of it.
    row("RT64 developer mode", "%s", yes_no(gfx.developer_mode));
}

void draw_draw_distance() {
    const DrawDistanceStatus dd = draw_distance_status();
    row("far plane scale", "%.2fx%s", double(dd.scale),
        dd.scale_from_env ? "  (pinned by RAYMAN2_DRAWDIST)" : "");
    // The three aspects are all the game's or the window's -- none of them is
    // "what the port supplied", because the port supplies none. See the note on
    // DrawDistanceStatus in include/debug_status.h.
    row("aspect: first asked", "%.4f", double(dd.first_aspect));
    row("aspect: last asked", "%.4f", double(dd.last_aspect));
    row("aspect: window", "%.4f", double(dd.window_aspect));
    // The one number that says whether the edge-culling fix is doing anything.
    // 1.00 means the camera is being left alone, which is right at 4:3 and
    // wrong on a widened window.
    row("fov widening", "%.4fx%s", double(dd.fov_widening),
        dd.fov_widening == 1.0f ? "  (off)" : "");
    row("pending projections", "%zu", dd.pending_projections);
}

void draw_memory_search() {
    const MemorySearchStatus ms = memory_search_status();
    if (!ms.enabled) {
        ImGui::TextDisabled("Off. Launch with RAYMAN2_MEMSEARCH=1 to arm it.");
        return;
    }
    row("armed", "yes");
    row("search started", "%s", yes_no(ms.started));
    if (ms.started) {
        row("candidates", "%zu", ms.candidates);
        row("narrowings", "%zu", ms.narrowings);
    }
    ImGui::TextDisabled("F5 start, then F6 (went down) / F7 (unchanged) / F8 (went up).");
}

void draw_demo_scan() {
    const DemoScanStatus scan = demo_scan_status();
    if (!scan.enabled) {
        ImGui::TextDisabled("Off. Launch with RAYMAN2_DEMOSCAN=1 to search for the");
        ImGui::TextDisabled("attract-mode flag. The flag it found is already in the port.");
        return;
    }
    row("samples idle / busy", "%d / %d", scan.idle_samples, scan.busy_samples);
    row("candidates", "%s",
        scan.have_candidates ? std::to_string(scan.candidates).c_str() : "(not yet built)");
}

void draw_capture() {
    const rayman2::capture::Status cap = rayman2::capture::status();
    row("captures this session", "%d", cap.taken);
    row_path("written to", cap.directory);
    ImGui::TextDisabled("F9 writes a screenshot, the raw N64 framebuffer, every");
    ImGui::TextDisabled("graphics setting and an issue stub, all at once.");
}

void draw_controller_pak() {
    // Only port 1 has one, and that is a decision rather than a limitation --
    // see docs/CONTROLLER-PAK-FINDINGS.md. Reporting all four would suggest
    // otherwise.
    // present() takes a zero-based port, and only port 0 -- the first
    // controller -- has a pak.
    row("port 1", "%s", rayman2::pak::present(0) ? "Controller Pak" : "nothing");
    ImGui::TextDisabled("A Controller Pak is presented instead of a Rumble Pak, so the");
    ImGui::TextDisabled("game can save. That is what leaves this port without rumble.");
}

void draw_window() {
    ImGui::SetNextWindowSize(ImVec2(560.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Rayman 2 Debug")) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Read-only. F1 closes this and RT64's Game editor together.");
    ImGui::Separator();

    // Open by default, because a readout nobody expands is a readout nobody
    // reads -- and every section here is a handful of lines.
    const ImGuiTreeNodeFlags open = ImGuiTreeNodeFlags_DefaultOpen;
    if (ImGui::CollapsingHeader("Session", open))          draw_session();
    if (ImGui::CollapsingHeader("Renderer", open))         draw_renderer();
    if (ImGui::CollapsingHeader("Graphics settings", open)) draw_graphics();
    if (ImGui::CollapsingHeader("Draw distance & widescreen", open)) draw_draw_distance();
    if (ImGui::CollapsingHeader("Capture", open))          draw_capture();
    if (ImGui::CollapsingHeader("Controller Pak", open))   draw_controller_pak();
    if (ImGui::CollapsingHeader("Memory search"))          draw_memory_search();
    if (ImGui::CollapsingHeader("Attract-mode scan"))      draw_demo_scan();

    ImGui::End();
}

}  // namespace

void init() {
    // On by default. This is part of the build rather than something a player
    // has to be told to switch on: the person who has just seen something odd
    // is running the game they downloaded, and telling them to fetch a
    // different one loses the thing they saw. It costs a null check per frame
    // while the menu is shut, because RT64 does not call the hook at all until
    // its own UI has been opened.
    const char* env = std::getenv("RAYMAN2_DEBUGMENU");
    g_enabled = (env == nullptr) || (std::strcmp(env, "0") != 0);
    g_started = clock_type::now();
    if (!g_enabled) {
        std::fprintf(stderr, "[rayman2] debug menu off (RAYMAN2_DEBUGMENU=0);"
                             " RT64's own is still on F1\n");
        std::fflush(stderr);
    }
}

void install() {
    if (!g_enabled) {
        return;
    }
    RT64_PortDebugMenuHook = draw_window;
}

}  // namespace rayman2::debug_menu
