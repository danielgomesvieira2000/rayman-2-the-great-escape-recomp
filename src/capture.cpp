// The F9 capture. See include/capture.h for what it is for and why.

#include "capture.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include "SDL.h"

#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"

#include "debug_report.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path g_dir;
std::atomic<int> g_count{0};

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &tm);
    return buf;
}

#ifdef _WIN32
// Grab the game window's client area to a 24-bit BMP.
//
// BMP rather than PNG on purpose: it needs no encoder and no dependency, and
// the file is opened once by a human and then attached to an issue. Paying a
// compression library for that would be the wrong trade.
bool write_window_bmp(const fs::path& path) {
    // The window with this process's foreground-most title. SDL owns it, but
    // the port has no handle to hand around here, and finding it is two calls.
    HWND window = nullptr;
    EnumWindows([](HWND candidate, LPARAM out) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(candidate, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(candidate)) {
            return TRUE;
        }
        char title[256] = {};
        if (GetWindowTextA(candidate, title, sizeof(title)) > 0) {
            *reinterpret_cast<HWND*>(out) = candidate;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&window));

    if (window == nullptr) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) {
        return false;
    }
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    HDC window_dc = GetDC(window);
    HDC memory_dc = CreateCompatibleDC(window_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(window_dc, width, height);
    HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    BitBlt(memory_dc, 0, 0, width, height, window_dc, 0, 0, SRCCOPY);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;   // positive: bottom-up, which BMP wants
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;

    const int stride = ((width * 3 + 3) / 4) * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
    const bool got_bits = GetDIBits(memory_dc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS) != 0;

    SelectObject(memory_dc, previous);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(window, window_dc);

    if (!got_bits) {
        return false;
    }

    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;   // "BM"
    file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    return out.good();
}
#endif

// Dump the framebuffer the VI is pointed at, straight out of RDRAM.
//
// This is a different picture from the screenshot whenever the renderer is
// scaling or widening, and the difference is diagnostic in itself: a defect
// present here came from the game or the display list, and one present only on
// screen came from the renderer.
bool write_vi_framebuffer(uint8_t* rdram, const fs::path& path, std::string& description) {
    const ultramodern::renderer::ViRegs* regs = ultramodern::renderer::get_vi_regs();
    if (regs == nullptr || rdram == nullptr || regs->VI_ORIGIN_REG == 0) {
        description = "no VI framebuffer (the game had not presented one)";
        return false;
    }

    const uint32_t origin = regs->VI_ORIGIN_REG & 0x03FFFFFFu;
    const uint32_t width = regs->VI_WIDTH_REG & 0xFFFu;
    // VI_STATUS bits 0-1 select the pixel size: 2 is 16-bit RGBA5551, 3 is
    // 32-bit RGBA8888. Anything else means the VI is blanked.
    const uint32_t pixel_size = regs->VI_STATUS_REG & 0x3u;
    const uint32_t bytes_per_pixel = (pixel_size == 3) ? 4 : 2;

    if (width == 0 || pixel_size < 2) {
        description = "VI is blanked; nothing to dump";
        return false;
    }

    // The VI gives the width directly but not the height: that has to be derived
    // from the vertical start/end pair and Y_SCALE, and the derivation is easy
    // to get subtly wrong -- an early version of this produced 297 lines for a
    // game that renders 224. So the height below is a best effort, it is bounded
    // to something a VI can actually scan out, and the raw registers are
    // recorded alongside it. Whoever converts the dump can then check the
    // arithmetic instead of inheriting it, which is the difference between
    // evidence and a claim.
    const uint32_t y_scale = regs->VI_Y_SCALE_REG & 0xFFFu;
    const uint32_t v_start = (regs->VI_V_START_REG >> 16) & 0x3FFu;
    const uint32_t v_end = regs->VI_V_START_REG & 0x3FFu;
    uint32_t height = 0;
    if (v_end > v_start && y_scale != 0) {
        height = ((v_end - v_start) >> 1) * y_scale / 1024;
    }
    if (height == 0 || height > 480) {
        height = 240;
    }

    const size_t bytes = static_cast<size_t>(width) * height * bytes_per_pixel;
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        description = "could not open the dump file";
        return false;
    }
    out.write(reinterpret_cast<const char*>(rdram + origin), static_cast<std::streamsize>(bytes));

    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "%u wide, %u bytes per pixel, from RDRAM 0x%08X. "
                  "Height %u is DERIVED, not read -- check it against the raw registers: "
                  "VI_STATUS=0x%08X VI_WIDTH=0x%08X VI_V_START=0x%08X VI_Y_SCALE=0x%08X "
                  "(the dump is width x that height, so the true height is also "
                  "file size / (width x bytes per pixel))",
                  width, bytes_per_pixel, origin, height,
                  regs->VI_STATUS_REG, regs->VI_WIDTH_REG,
                  regs->VI_V_START_REG, regs->VI_Y_SCALE_REG);
    description = buf;
    return out.good();
}

const char* name_of(ultramodern::renderer::AspectRatio value) {
    switch (value) {
        case ultramodern::renderer::AspectRatio::Original: return "Original (4:3)";
        case ultramodern::renderer::AspectRatio::Expand:   return "Expand (widescreen)";
        case ultramodern::renderer::AspectRatio::Manual:   return "Manual";
        default:                                           return "?";
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

const char* name_of(ultramodern::renderer::RefreshRate value) {
    switch (value) {
        case ultramodern::renderer::RefreshRate::Original: return "Original (the game's own rate)";
        case ultramodern::renderer::RefreshRate::Display:  return "Display";
        case ultramodern::renderer::RefreshRate::Manual:   return "Manual";
        default:                                           return "?";
    }
}

} // namespace

namespace rayman2::capture {

void set_output_directory(const fs::path& debug_report_dir) {
    if (debug_report_dir.empty()) {
        return;
    }
    std::error_code ec;
    g_dir = debug_report_dir / "captures";
    fs::create_directories(g_dir, ec);
}

void take(uint8_t* rdram, const char* reason) {
    if (g_dir.empty()) {
        return;
    }

    const int index = g_count.fetch_add(1) + 1;
    const std::string stem = timestamp() + "-" + std::to_string(index);
    const fs::path dir = g_dir / stem;
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::string screenshot_note = "not captured";
#ifdef _WIN32
    screenshot_note = write_window_bmp(dir / "screen.bmp") ? "screen.bmp"
                                                           : "failed to capture the window";
#endif

    std::string framebuffer_note;
    const bool got_fb = write_vi_framebuffer(rdram, dir / "vi-framebuffer.bin", framebuffer_note);

    const auto& gfx = ultramodern::renderer::get_graphics_config();

    std::ofstream stub(dir / "ISSUE.md");
    if (stub) {
        stub << "# Graphics issue: (one line -- what looks wrong)\n"
             << "\n"
             << "Captured by " << reason << " on " << timestamp() << ".\n"
             << "\n"
             << "## What is wrong\n"
             << "\n"
             << "(Replace this. Say what you see and what you expected instead. If a real\n"
             << "N64 or an emulator looks different at this spot, say so -- \"is this a port\n"
             << "bug or is this how the game looked\" is a real question and it is expensive\n"
             << "to answer from a screenshot alone.)\n"
             << "\n"
             << "## Where\n"
             << "\n"
             << "(Level, and how to get to this exact spot. A Controller Pak image parked\n"
             << "just before it is worth more than any description -- copy\n"
             << "controller_pak_1.pak from the config folder next to this file.)\n"
             << "\n"
             << "## Is it stable?\n"
             << "\n"
             << "- [ ] every frame, or only sometimes?\n"
             << "- [ ] only when the camera moves?\n"
             << "- [ ] only at this spot, or everywhere of this kind?\n"
             << "\n"
             << "## The three-toggle triage\n"
             << "\n"
             << "Change one at a time in the Graphics tab and note what happens. This\n"
             << "eliminates most of the search space before anyone reads a line of code.\n"
             << "\n"
             << "- Internal resolution: (scales with it? then it is a renderer/upscaling issue)\n"
             << "- Aspect ratio Expand <-> Original: (only when widened? then it is culling or 2D anchoring)\n"
             << "- MSAA off: (gone? then it is a coverage/edge issue)\n"
             << "\n"
             << "## The frame itself\n"
             << "\n"
             << "Relaunch with RAYMAN2_DEVMODE=1 and press F1 to open RT64's\n"
             << "developer UI (there is no checkbox for it). Pause on the bad frame and\n"
             << "walk the draw calls; a screenshot of that panel, or just \"draw call N in projection M is the\n"
             << "thing that looks wrong\", turns a day of guessing into an hour.\n"
             << "\n"
             << "## Captured automatically\n"
             << "\n"
             << "- screenshot: " << screenshot_note << "\n"
             << "- VI framebuffer: " << (got_fb ? "vi-framebuffer.bin, " + framebuffer_note
                                                : framebuffer_note) << "\n"
             << "  (raw pixels as the game left them, before any scaling or widening --\n"
             << "   convert with tools/fb_to_png.py. A defect visible here came from the\n"
             << "   game or the display list; one visible only in the screenshot came from\n"
             << "   the renderer.)\n"
             << "\n"
             << "### Graphics settings in force\n"
             << "\n"
             << "- aspect ratio: " << name_of(gfx.ar_option) << "\n"
             << "- HUD ratio: " << name_of(gfx.hr_option) << "\n"
             << "- antialiasing: " << name_of(gfx.msaa_option) << "\n"
             << "- refresh rate: " << name_of(gfx.rr_option)
             << " (manual value " << gfx.rr_manual_value << ")\n"
             << "- downsampling: " << gfx.ds_option << "\n"
             << "- developer mode: " << (gfx.developer_mode ? "on" : "off") << "\n"
             << "\n"
             << "### Session\n"
             << "\n"
             << "The session report for this run is the .txt one directory up. Its event\n"
             << "log covers the moments before this capture, which is often where the\n"
             << "explanation is.\n";
    }

    rayman2::report::info("capture", "wrote %s", dir.string().c_str());
    std::fprintf(stderr, "[rayman2] capture written to %s\n", dir.string().c_str());
}

void poll_hotkey(uint8_t* rdram) {
    if (g_dir.empty()) {
        return;
    }

    // F9, because RT64 has already taken F1 to F4 for its developer shortcuts
    // and colliding with the frame inspector would be a poor joke.
    int num_keys = 0;
    const Uint8* keys = SDL_GetKeyboardState(&num_keys);
    if (keys == nullptr || num_keys <= SDL_SCANCODE_F9) {
        return;
    }

    static bool was_down = false;
    const bool is_down = keys[SDL_SCANCODE_F9] != 0;
    if (is_down && !was_down) {
        take(rdram, "F9");
    }
    was_down = is_down;
}

} // namespace rayman2::capture
