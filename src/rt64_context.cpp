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
#include <cstdint>
#include <cstdio>
#include <memory>

#include "hle/rt64_application.h"

#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"

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

    bool valid() override { return app != nullptr; }

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
    std::unique_ptr<RT64::Application> app;
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
    app->userConfig.developerMode  = developer_mode;

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
    // The game's display list, handed over by librecomp instead of being run on
    // an emulated RSP. Phase 00 established this game uses stock F3DEX.NoN 1.23,
    // which RT64 identifies from the microcode address below.
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
}

void RT64Context::send_dummy_workload(uint32_t fb_address) {
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
    app->updateScreen();
}

void RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                const ultramodern::renderer::GraphicsConfig& new_config) {
    if (app == nullptr || old_config == new_config) {
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
    if (app != nullptr && app->appWindow != nullptr) {
        const uint32_t rate = app->appWindow->getRefreshRate();
        if (rate != 0) {
            return rate;
        }
    }
    return 60;   // NTSC fallback until the real rate is known
}

float RT64Context::get_resolution_scale() const {
    if (app != nullptr && app->userConfig.resolution == RT64::UserConfiguration::Resolution::Manual) {
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
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}

} // namespace rayman2
