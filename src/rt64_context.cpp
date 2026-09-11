// A minimal RT64 renderer context owned by the port.
//
// WHY THIS EXISTS
//
// RecompFrontend ships a complete RT64 context, and src/render_context.cpp uses
// it when the frontend is enabled. That is the right answer for a finished
// port, but it couples two independent questions together: "does the recompiled
// game run" and "does the menu system initialise". Phase 03's gate is only the
// first of those, and it was blocked entirely by the second -- an exception
// thrown on the renderer thread during recompui's bring-up, which crosses a
// noexcept boundary and fail-fasts the process with no message.
//
// So this file exists to make the game bootable without any UI at all. It
// implements ultramodern's RendererContext directly on RT64::Application and
// nothing else.
//
// WHAT "MINIMAL" MEANS HERE
//
// It deliberately does NOT carry the accumulated workarounds a mature port
// collects -- deferring MSAA changes to the next launch, routing the pause
// state into RT64, choosing a presentation mode to suit a particular game's
// menu transitions. Those are real findings, but they are findings about other
// games, and copying them here would be cargo cult: they would be untestable
// against this game until it renders, and indistinguishable from deliberate
// choices once it does. They belong in phase 05, added one at a time with a
// reason, if this game turns out to need them.
//
// What it does keep is honest failure reporting. RT64 setup failing after the
// window is already open is exactly the case that otherwise presents as a black
// window and no explanation.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "hle/rt64_application.h"

#include "debug_menu.h"
#include "debug_status.h"

#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

namespace rayman2 {

    // src/demo_scan.cpp
    void note_display_list();

    // Defined here rather than in main.cpp so that the only thing that can set
    // it is the renderer actually running a frame.
    std::atomic<bool>& vi_has_ticked() {
        static std::atomic<bool> ticked{false};
        return ticked;
    }
}

namespace {

// RT64 wants pointers to the RCP's register file. The recompiled game never
// drives the RDP directly -- librecomp intercepts the display-list task and
// hands it to send_dl below -- so these exist to be pointed at and are
// otherwise inert. The VI registers are the exception: those are real, and come
// from ultramodern, which is what makes the picture track the game's own video
// mode.
uint8_t  s_dmem[0x1000]{};
uint8_t  s_imem[0x1000]{};
uint32_t s_mi_intr_reg = 0;
uint32_t s_dpc_start_reg = 0, s_dpc_end_reg = 0, s_dpc_current_reg = 0, s_dpc_status_reg = 0;
uint32_t s_dpc_clock_reg = 0, s_dpc_bufbusy_reg = 0, s_dpc_pipebusy_reg = 0, s_dpc_tmem_reg = 0;

// ---------------------------------------------------------------------------
// Boot instrumentation.
//
// The game runs but the screen is black, and there are three possible reasons
// that look identical from outside: it is stuck in an early wait loop and has
// not reached drawing; it is running its main loop but nothing reaches the
// renderer; or display lists do arrive and RT64 draws nothing recognisable.
//
// These counters separate all three in a single run. update_screen ticks once
// per VI, so frames counts whether the video path is alive at all, and
// display_lists counts whether the game is actually submitting graphics:
//
//   frames 0                 -> the VI thread is not running; look there
//   frames rising, lists 0   -> the game is not submitting; it is stuck earlier
//   both rising              -> submission works; the problem is in drawing
//
// Set RAYMAN2_QUIET_BOOT to silence the periodic line once it has served.
// ---------------------------------------------------------------------------
std::atomic<uint64_t> g_display_lists{0};
std::atomic<uint64_t> g_dummy_workloads{0};
std::atomic<uint64_t> g_frames{0};

bool quiet_boot() {
    static const bool quiet = std::getenv("RAYMAN2_QUIET_BOOT") != nullptr;
    return quiet;
}

// RT64 calls this when it would raise an RCP interrupt. Nothing here needs to
// know: ultramodern owns interrupt scheduling.
void no_check_interrupts() {}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult r) {
    switch (r) {
        case RT64::Application::SetupResult::Success:                  return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:       return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:      return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:   return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:  return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:  return ultramodern::renderer::GraphicsApi::Metal;
        default:                                           return ultramodern::renderer::GraphicsApi::Auto;
    }
}

RT64::UserConfiguration::GraphicsAPI to_rt64(ultramodern::renderer::GraphicsApi api) {
    switch (api) {
        case ultramodern::renderer::GraphicsApi::D3D12:  return RT64::UserConfiguration::GraphicsAPI::D3D12;
        case ultramodern::renderer::GraphicsApi::Vulkan: return RT64::UserConfiguration::GraphicsAPI::Vulkan;
        case ultramodern::renderer::GraphicsApi::Metal:  return RT64::UserConfiguration::GraphicsAPI::Metal;
        default:                                          return RT64::UserConfiguration::GraphicsAPI::Automatic;
    }
}

const char* describe(ultramodern::renderer::SetupResult r) {
    switch (r) {
        case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
            return "required graphics libraries were not found "
                   "(on Windows, dxcompiler.dll and dxil.dll must sit next to the executable)";
        case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
            return "the configured graphics API is not valid for this platform";
        case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
            return "no supported graphics API is available "
                   "(on Linux this usually means the Vulkan loader, libvulkan, is missing)";
        case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
            return "no compatible GPU was found; update the graphics driver";
        default:
            return "unknown error";
    }
}

class RT64Context final : public ultramodern::renderer::RendererContext {
public:
    RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
    ~RT64Context() override = default;

    bool valid() override { return usable(); }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override;
    void enable_instant_present() override;
    void send_dl(const OSTask* task) override;
    void send_dummy_workload(uint32_t fb_address) override;
    void update_screen() override;
    void shutdown() override;
    uint32_t get_display_framerate() const override;
    float get_resolution_scale() const override;

private:
    // Every entry point below is called from ultramodern's threads, not from
    // this object's owner, and two of those calls can arrive when there is
    // nothing to call into: before setup succeeded, and after shutdown() has
    // ended the application. A null check alone only covers the first -- the
    // second leaves `app` non-null but no longer usable, which is a
    // use-after-end that reads exactly like a working pointer.
    bool usable() const {
        return app != nullptr && !ended.load(std::memory_order_acquire);
    }

    std::unique_ptr<RT64::Application> app;

    // Set by shutdown(). Atomic because shutdown() and the renderer thread do
    // not otherwise synchronise with each other.
    std::atomic<bool> ended{false};
};

RT64Context::RT64Context(uint8_t* rdram,
                         ultramodern::renderer::WindowHandle window_handle,
                         bool developer_mode) {
    // RT64 reads the cartridge header to identify the game for its per-title
    // configuration and texture packs. This port hands it zeroes: nothing here
    // depends on RT64 recognising the game, and giving it the real header would
    // mean copying ROM bytes into the renderer for no benefit.
    static uint8_t blank_rom_header[0x40]{};

    RT64::Application::Core core{};
#if defined(_WIN32)
    core.window = window_handle.window;
#else
    core.window = window_handle;
#endif
    core.checkInterrupts = no_check_interrupts;
    core.HEADER = blank_rom_header;
    core.RDRAM  = rdram;
    core.DMEM   = s_dmem;
    core.IMEM   = s_imem;
    core.MI_INTR_REG      = &s_mi_intr_reg;
    core.DPC_START_REG    = &s_dpc_start_reg;
    core.DPC_END_REG      = &s_dpc_end_reg;
    core.DPC_CURRENT_REG  = &s_dpc_current_reg;
    core.DPC_STATUS_REG   = &s_dpc_status_reg;
    core.DPC_CLOCK_REG    = &s_dpc_clock_reg;
    core.DPC_BUFBUSY_REG  = &s_dpc_bufbusy_reg;
    core.DPC_PIPEBUSY_REG = &s_dpc_pipebusy_reg;
    core.DPC_TMEM_REG     = &s_dpc_tmem_reg;

    // The VI registers are ultramodern's, not ours: the game writes its video
    // mode through libultra, and RT64 reads it here.
    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    core.VI_STATUS_REG         = &vi->VI_STATUS_REG;
    core.VI_ORIGIN_REG         = &vi->VI_ORIGIN_REG;
    core.VI_WIDTH_REG          = &vi->VI_WIDTH_REG;
    core.VI_INTR_REG           = &vi->VI_INTR_REG;
    core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
    core.VI_TIMING_REG         = &vi->VI_TIMING_REG;
    core.VI_V_SYNC_REG         = &vi->VI_V_SYNC_REG;
    core.VI_H_SYNC_REG         = &vi->VI_H_SYNC_REG;
    core.VI_LEAP_REG           = &vi->VI_LEAP_REG;
    core.VI_H_START_REG        = &vi->VI_H_START_REG;
    core.VI_V_START_REG        = &vi->VI_V_START_REG;
    core.VI_V_BURST_REG        = &vi->VI_V_BURST_REG;
    core.VI_X_SCALE_REG        = &vi->VI_X_SCALE_REG;
    core.VI_Y_SCALE_REG        = &vi->VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration app_config;
    // Do not read or write RT64's own configuration file. The port owns its
    // settings; letting RT64 persist a parallel set of them behind our back
    // makes behaviour depend on invisible state from a previous run.
    app_config.useConfigurationFile = false;

    app = std::make_unique<RT64::Application>(core, app_config);

    const ultramodern::renderer::GraphicsConfig& cfg = ultramodern::renderer::get_graphics_config();
    app->userConfig.graphicsAPI    = to_rt64(cfg.api_option);
    // Developer mode is forced on, exactly as the frontend build forces it in
    // src/render_context.cpp, and for the same reason: F1 is the debug menu, and
    // every path to it -- the Win32 subclass and SDL event filter RT64 installs,
    // the key handler, State::inspect() at the far end -- is gated on this one
    // flag, which is read here and never looked at again. A build where the menu
    // cannot be opened is a build where the menu does not exist.
    //
    // RAYMAN2_DEVMODE=0 turns it off, the same spelling as the other build.
    const char* devmode_env = std::getenv("RAYMAN2_DEVMODE");
    const bool developer_disabled =
        (devmode_env != nullptr) && (std::strcmp(devmode_env, "0") == 0);
    (void)developer_mode;
    app->userConfig.developerMode  = !developer_disabled;

    // The port's own window inside that UI. A no-op when the menu is off.
    rayman2::debug_menu::install();

    uint32_t thread_id = 0;
#if defined(_WIN32)
    thread_id = window_handle.thread_id;
#endif

    setup_result = map_setup_result(app->setup(thread_id));
    chosen_api   = map_graphics_api(app->chosenGraphicsAPI);

    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        // The window is already open by this point, so a silent failure here is
        // a black window and nothing else. Name the likely cause.
        std::fprintf(stderr, "[rayman2] RT64 setup failed: %s (result %d)\n",
                     describe(setup_result), static_cast<int>(setup_result));
        app = nullptr;
        return;
    }

    app->setFullScreen(cfg.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    std::fprintf(stderr, "[rayman2] RT64 ready (graphics api %d)\n", static_cast<int>(chosen_api));
}

void RT64Context::send_dl(const OSTask* task) {
    if (!usable()) {
        return;
    }
    const uint64_t n = g_display_lists.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1) {
        std::fprintf(stderr,
                     "[rayman2] FIRST DISPLAY LIST after %llu frames -- the game is drawing\n",
                     static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed)));
    }
    // The game's display list, handed over by librecomp instead of being run on
    // an emulated RSP. Phase 00 established this game uses stock F3DEX.NoN 1.23,
    // which RT64 identifies from the microcode address below.
    // Report the graphics microcode once, so which GBI RT64 is handed is a
    // matter of record rather than of phase 00's reading of the ROM.
    if (n == 1) {
        std::fprintf(stderr,
                     "[rayman2] GFX ucode=0x%08X/0x%X data=0x%08X/0x%X boot=0x%08X/0x%X\n",
                     (unsigned)task->t.ucode, (unsigned)task->t.ucode_size,
                     (unsigned)task->t.ucode_data, (unsigned)task->t.ucode_data_size,
                     (unsigned)task->t.ucode_boot, (unsigned)task->t.ucode_boot_size);
    }
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    rayman2::note_display_list();
}

void RT64Context::send_dummy_workload(uint32_t fb_address) {
    if (!usable()) {
        return;
    }
    g_dummy_workloads.fetch_add(1, std::memory_order_relaxed);
    // Give the VI something real to present before the game submits its first
    // display list: a fill of the whole 320x240 framebuffer through the RDP.
    // Without it the first frames present whatever RDRAM happens to contain.
    app->state->listProcessBegin();
    app->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
    // Fill cycle, no alpha compare, no z, no blending -- the RDP's other-mode
    // word for "just write the fill colour".
    app->state->rdp->setOtherMode(0x382C30, 0);
    app->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
    app->state->fullSync();
    app->state->listProcessEnd();
}

void RT64Context::update_screen() {
    if (!usable()) {
        return;
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    rayman2::note_presented_frame();
    if (!quiet_boot()) {
        // update_screen is the VI thread and nothing else, so this bookkeeping
        // needs no synchronisation of its own.
        using clock = std::chrono::steady_clock;
        static clock::time_point last = clock::now();
        static uint64_t last_frames = 0, last_lists = 0, last_dummies = 0;

        const clock::time_point now = clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            const uint64_t f = g_frames.load(std::memory_order_relaxed);
            const uint64_t d = g_display_lists.load(std::memory_order_relaxed);
            const uint64_t u = g_dummy_workloads.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[rayman2] frames %llu (+%llu/s)  display lists %llu (+%llu/s)  dummy %llu (+%llu/s)\n",
                         static_cast<unsigned long long>(f),
                         static_cast<unsigned long long>(f - last_frames),
                         static_cast<unsigned long long>(d),
                         static_cast<unsigned long long>(d - last_lists),
                         static_cast<unsigned long long>(u),
                         static_cast<unsigned long long>(u - last_dummies));
            last = now;
            last_frames = f; last_lists = d; last_dummies = u;
        }
    }

    // Publish that the VI thread has run at least once. main() waits on this
    // before starting the game: ultramodern's VI thread only seeds a video mode
    // while the game has not started, so starting first races it and the update
    // path dereferences a mode that is not there yet.
    rayman2::vi_has_ticked().store(true, std::memory_order_release);

    // Report and dump the framebuffer the VI is pointed at (RAYMAN2_FBPROBE).
    //
    // This exists because the display-list counter cannot answer the question
    // it looks like it answers. The counter read zero all the way through the
    // Controller Pak prompt, which was taken as "the prompt does not render" --
    // and that was wrong. The game draws that screen with the CPU, straight
    // into RDRAM, submitting no display list at all, and RT64 presents it
    // perfectly. A counter of RDP work says nothing about a picture drawn
    // without the RDP.
    //
    // So: this says what is in the framebuffer, and tools/grab_window.ps1 says
    // what is on the screen. Between them a black window can be attributed to
    // the game drawing nothing or to the port failing to present something,
    // which is a distinction no counter here can make.
    static const bool fb_probe = std::getenv("RAYMAN2_FBPROBE") != nullptr;
    if (fb_probe) {
        using clock = std::chrono::steady_clock;
        static clock::time_point last_fb = clock::now();
        const clock::time_point now_fb = clock::now();
        if (now_fb - last_fb >= std::chrono::seconds(1)) {
            last_fb = now_fb;
            const ultramodern::renderer::ViRegs* regs = ultramodern::renderer::get_vi_regs();
            const uint32_t origin = regs != nullptr ? regs->VI_ORIGIN_REG : 0u;
            uint64_t nonzero = 0, hash = 1469598103934665603ull;
            if (origin != 0 && app != nullptr && app->core.RDRAM != nullptr) {
                const uint8_t* fb = app->core.RDRAM + (origin & 0x3FFFFFF);
                for (uint32_t i = 0; i < 320 * 240 * 2; ++i) {
                    if (fb[i] != 0) ++nonzero;
                    hash = (hash ^ fb[i]) * 1099511628211ull;
                }
            }
            std::fprintf(stderr, "[rayman2] fb origin=0x%08X nonzero=%llu hash=%016llx\n",
                         origin, (unsigned long long)nonzero, (unsigned long long)hash);

            // Write the raw framebuffer out so it can actually be looked at.
            //
            // Nothing else in this port can answer "what is on the screen". The
            // counters say a display list was submitted, not what it drew, and
            // a black window is equally consistent with the game drawing
            // nothing and with the port failing to present what it drew. The
            // bytes settle it; tools/fb_to_png.py makes them viewable.
            if (origin != 0 && app != nullptr && app->core.RDRAM != nullptr) {
                if (std::FILE* f = std::fopen("fb_dump.bin", "wb")) {
                    std::fwrite(app->core.RDRAM + (origin & 0x3FFFFFF), 1, 320 * 240 * 2, f);
                    std::fclose(f);
                }
            }
        }
    }

    app->updateScreen();
}

void RT64Context::shutdown() {
    // Mark it unusable first. Ending the application while another thread is
    // part-way through updateScreen() is the same defect as calling into it
    // afterwards, and setting the flag first closes the window in which a call
    // can start.
    if (ended.exchange(true, std::memory_order_acq_rel)) {
        return;   // already shut down
    }
    if (app != nullptr) {
        app->end();
    }
}

bool RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                const ultramodern::renderer::GraphicsConfig& new_config) {
    if (!usable() || old_config == new_config) {
        return false;
    }
    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }
    app->updateUserConfig(true);
    return true;
}

void RT64Context::enable_instant_present() {
    // Called once per session. Presentation mode is left at RT64's default:
    // choosing between Console, SkipBuffering and PresentEarly is a decision
    // about how a specific game's frames reach the screen, and this game has
    // not drawn one yet. Phase 05 is where that gets decided, with something to
    // look at.
}

uint32_t RT64Context::get_display_framerate() const {
    if (usable() && app->appWindow != nullptr) {
        const uint32_t rate = app->appWindow->getRefreshRate();
        if (rate != 0) {
            return rate;
        }
    }
    return 60;   // NTSC fallback until the real rate is known
}

float RT64Context::get_resolution_scale() const {
    if (usable() && app->userConfig.resolution == RT64::UserConfiguration::Resolution::Manual) {
        return static_cast<float>(app->userConfig.resolutionMultiplier);
    }
    return 1.0f;
}

} // namespace

namespace rayman2 {

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram,
                      ultramodern::renderer::WindowHandle window_handle,
                      bool developer_mode) {
    // This is the only place the port is handed librecomp's RDRAM base, so it
    // is the only place that can report it. Worth printing: the crash under
    // investigation writes to an address VirtualQuery calls FREE, and knowing
    // the base says whether that address is a bad offset from a good base or a
    // good offset from a bad base.
    std::fprintf(stderr, "[rayman2] rdram base = %p\n", static_cast<void*>(rdram));
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}

} // namespace rayman2
