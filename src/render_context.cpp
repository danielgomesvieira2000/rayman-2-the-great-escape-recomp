// Renderer bring-up: hand ultramodern the RT64 context that RecompFrontend owns.
//
// The port does NOT implement its own RT64 context. RecompFrontend ships one
// (recompui::renderer::create_render_context), because its menus are drawn as
// an overlay on top of the game and it therefore has to own the RT64
// application to attach its render hooks to. Supplying a second, separate
// context here would mean either no menus or two renderers fighting over the
// same window.
//
// The only mismatch is arity: ultramodern's callback passes three arguments,
// while recompui's factory takes a presentation mode as well. This adapter
// supplies it.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#ifdef _WIN32
#include <objbase.h>
#endif

#include "rhi/rt64_render_hooks.h"

// src/draw_distance.cpp and src/main.cpp
namespace rayman2 {
    void narrow_pending_projections(uint8_t* rdram);
    uint8_t* rdram_base();


    // src/frame_pacing.cpp -- the presentation half of RAYMAN2_PACEPROBE.
    void note_presented();

    // Set once the renderer has actually presented a frame.
    //
    // src/rt64_context.cpp -- the non-frontend renderer -- has carried this
    // since phase 03, where starting the game before the renderer's first tick
    // made the VI update path dereference a video mode that did not exist yet.
    // This build had the declaration in main.cpp and no definition anywhere,
    // which linked only because nothing in a frontend build referred to it.
    // Defined here rather than in main.cpp for the same reason as there: the
    // only thing that should be able to set it is a frame having been drawn.
    std::atomic<bool>& vi_has_ticked() {
        static std::atomic<bool> ticked{false};
        return ticked;
    }
}

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recompui/renderer.h"

namespace {

// Make the launcher's "Load ROM" file dialog work.
//
// The chain is: the player clicks the option, RmlUi dispatches the element
// callback, recompui calls NFD_OpenDialogN, and NFD calls
// CoCreateInstance(CLSID_FileOpenDialog). COM is per-thread, and nothing in
// RecompFrontend, librecomp, ultramodern or RT64 ever calls NFD_Init or
// CoInitializeEx -- the frontend leaves that to the port. Without it
// CoCreateInstance returns CO_E_NOTINITIALIZED, NFD returns NFD_ERROR, and
// recompui treats that exactly like the player pressing Cancel. Clicking
// "Load ROM" therefore highlighted the option and did nothing at all: no
// dialog, no error, no log line.
//
// The thread it has to happen on is not an obvious one, and guessing costs a
// build each time. Three separate threads are involved, and instrumenting all
// three was the only way to tell them apart:
//
//     main thread                 runs main() and recomp::start
//     gfx thread                  runs ultramodern's gfx_thread_func, which is
//                                 what calls this very function
//     RT64 present-queue thread   runs the render hooks -- and the menus
//
// recompui draws its menus from RT64's draw hook (rt64_present_queue.cpp), and
// it also dequeues and dispatches input events there, so the click callback --
// and the dialog -- runs on the present-queue thread. Initialising COM in
// main(), or here in the renderer factory on the gfx thread, leaves that thread
// untouched and changes nothing; both were tried first and neither moved the
// probe off "Could not create dialog."
//
// So wrap RT64's draw hook. recompui installs its hooks at the top of its
// RT64Context constructor, before the application exists, so by the time the
// factory below returns they are in place and can be read back and chained.
// The initialisation is thread_local, which is the point: it runs once on
// whichever thread RT64 ends up calling the hook from, without this code having
// to know which thread that is.

// Which presentation mode to run in.
//
// Console is the default and the only one this game has been shown to render
// correctly. It presents what the VI points at, exactly as the console does.
//
// The other two present the framebuffer the game has just drawn instead, and
// that is what RT64 requires before it will interpolate: PresentQueue only sets
// interpolationEnabled when the presented framebuffer is one the workload
// modified this frame, which under Console is never true for a double-buffered
// game. They also present it while the game may still be drawing into it, and
// on Rayman 2 that shows: captured frames under SkipBuffering had whole chunks
// of the scene missing and the HUD digits sliced off. It is not subtle and it
// is not rare, so it cannot be the default.
//
// It stays reachable because the interpolation it unlocks cannot be evaluated
// on a 60 Hz display by a game that already reaches 60. On a faster panel it is
// worth measuring -- with RAYMAN2_FPSPROBE=1 to see the rate and both eyes on
// the picture to see the cost.
ultramodern::renderer::PresentationMode presentation_mode() {
    const char* requested = std::getenv("RAYMAN2_PRESENT");
    if (requested != nullptr) {
        if (std::strcmp(requested, "skipbuffering") == 0) {
            std::fprintf(stderr, "[rayman2] presentation: SkipBuffering (interpolation possible; "
                                 "expect torn frames)\n");
            return ultramodern::renderer::PresentationMode::SkipBuffering;
        }
        if (std::strcmp(requested, "presentearly") == 0) {
            std::fprintf(stderr, "[rayman2] presentation: PresentEarly (interpolation possible; "
                                 "expect torn frames)\n");
            return ultramodern::renderer::PresentationMode::PresentEarly;
        }
    }
    return ultramodern::renderer::PresentationMode::Console;
}

RT64::RenderHookDraw* recompui_draw_hook = nullptr;

// Count presented frames.
//
// RT64 calls the draw hook once per frame it actually presents, so this counts
// what the player sees rather than what the game drew -- which is the whole
// point when the renderer is generating frames between the game's own. Compare
// it against RAYMAN2_AUDIOPROBE, which reports the rate the GAME is producing
// audio at: if this number rises while that one does not, the extra frames are
// coming from interpolation and the simulation is running at its original
// speed. If both rise, something has sped the game up and the physics with it.
void count_presented_frame() {
    static const bool enabled = std::getenv("RAYMAN2_FPSPROBE") != nullptr;
    if (!enabled) {
        return;
    }
    using clock = std::chrono::steady_clock;
    static clock::time_point window_start = clock::now();
    static int frames = 0;

    frames++;
    const clock::time_point now = clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start).count();
    if (elapsed >= 1000) {
        // The display rate is the one RT64 measured from the swap chain, and it
        // is what RefreshRate::Display makes the interpolation target. If it
        // does not match the monitor, no amount of interpolation will either.
        std::fprintf(stderr, "[rayman2] presented %.1f frames/s   (display reports %u Hz)\n",
                     frames * 1000.0 / static_cast<double>(elapsed),
                     ultramodern::get_display_refresh_rate());
        frames = 0;
        window_start = now;
    }
}

void draw_hook_with_com(RenderCommandList* list, RenderFramebuffer* swap_chain_framebuffer) {
    rayman2::vi_has_ticked().store(true, std::memory_order_release);
    rayman2::note_presented();
    count_presented_frame();

#ifdef _WIN32
    // Apartment-threaded, matching what NFD_Init would request. There is no
    // matching CoUninitialize: this thread lives as long as the renderer.
    static thread_local const bool com_ready = []() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        // RPC_E_CHANGED_MODE means COM is already up on this thread in the
        // other mode, which is fine. Anything else is worth saying out loud,
        // because the symptom is otherwise a dead menu item.
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            std::fprintf(stderr, "[rayman2] CoInitializeEx failed (0x%08lX); "
                                 "the Load ROM dialog will not open\n",
                         static_cast<unsigned long>(hr));
        }
        return true;
    }();
    (void)com_ready;
#endif

    if (recompui_draw_hook != nullptr) {
        recompui_draw_hook(list, swap_chain_framebuffer);
    }
}

} // namespace


// A thin shell around recompui's renderer context, for one job.
//
// The game and RT64 need different values out of the same projection matrix.
// The game culls against it while it builds a frame, and wants it widened so
// that what it keeps matches the widescreen frame the player sees; RT64 reads
// it afterwards, when it parses the display list, and wants the narrow one
// because its own Expand multiplies it up to produce the displayed field of
// view. Give either one the other's matrix and you get a third too much or a
// third too little.
//
// They read it at different times, so both can have what they need. The matrix
// stays wide for the whole of the game's frame and is narrowed here, at the
// handover -- which is why this exists at all, and why it overrides exactly one
// method and forwards the rest.
//
// The base class keeps setup_result and chosen_api as protected members with
// default getters that read them, so those two have to be forwarded explicitly
// or callers get this shell's uninitialised copies rather than the real
// context's answers.
class WidescreenCullingContext final : public ultramodern::renderer::RendererContext {
public:
    explicit WidescreenCullingContext(std::unique_ptr<ultramodern::renderer::RendererContext> inner)
        : inner_(std::move(inner)) {}

    bool valid() override { return inner_->valid(); }
    ultramodern::renderer::SetupResult get_setup_result() const override { return inner_->get_setup_result(); }
    ultramodern::renderer::GraphicsApi get_chosen_api() const override { return inner_->get_chosen_api(); }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        return inner_->update_config(old_config, new_config);
    }

    void enable_instant_present() override { inner_->enable_instant_present(); }

    void send_dl(const OSTask* task) override {
        rayman2::narrow_pending_projections(rayman2::rdram_base());
        inner_->send_dl(task);
    }

    void send_dummy_workload(uint32_t fb_address) override { inner_->send_dummy_workload(fb_address); }
    void update_screen() override { inner_->update_screen(); }
    void shutdown() override { inner_->shutdown(); }
    uint32_t get_display_framerate() const override { return inner_->get_display_framerate(); }
    float get_resolution_scale() const override { return inner_->get_resolution_scale(); }

private:
    std::unique_ptr<ultramodern::renderer::RendererContext> inner_;
};

namespace rayman2 {


std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram,
                      ultramodern::renderer::WindowHandle window_handle,
                      bool developer_mode) {
    // Console presentation matches the cartridge's own pacing: a frame is shown
    // when the game asks for it, rather than the renderer running ahead. It is
    // the conservative choice for bring-up, because the alternatives change
    // when frames appear relative to the game's own timing, and phase 04 needs
    // to be able to trust that what is on screen is what the game just drew.
    // Revisit alongside the high-framerate work in phase 06.
    // Developer mode, and why the port has to be able to force it.
    //
    // RT64's frame inspector (F1) is the tool for a graphics bug, and reaching
    // it takes two things that are both easy to miss.
    //
    // RecompFrontend registers "Dev Mode" with hidden = true, so it is never
    // drawn in the Graphics tab: there is no checkbox to find, only a
    // developer_mode key in graphics.json.
    //
    // And it cannot be turned on while the game is running. RT64 installs the
    // Win32 message hook that delivers F1 during ApplicationWindow setup, gated
    // on Application::usesWindowMessageFilter(), which returns
    // userConfig.developerMode -- a value written once, in the RT64Context
    // constructor, from the argument below. Flip the config afterwards and the
    // hook was never installed, so the key goes nowhere and it looks like the
    // feature does not exist.
    //
    // So it has to be decided here, before the renderer is built. The
    // environment variable is the convenient way in and leaves no trace in the
    // saved configuration; the graphics.json key still works for anyone who
    // wants it on permanently.
    // ON by default in this release.
    //
    // 0.2 is a playtesting build: the whole point is that when something looks
    // wrong the tools to describe it are already there, rather than needing a
    // relaunch with an environment variable set -- by which time the moment has
    // gone. F1 opens the frame inspector, F3 views RDRAM, F4 pauses. It costs a
    // little overhead and an ImGui overlay that only appears when asked.
    //
    // RAYMAN2_DEVMODE=0 turns it off, and wins over the graphics.json flag.
    const char* devmode_env = std::getenv("RAYMAN2_DEVMODE");
    const bool developer_disabled = (devmode_env != nullptr) && (std::strcmp(devmode_env, "0") == 0);
    // The environment variable wins outright, in both directions: an override
    // that can be silently overridden is not an override.
    const bool developer = !developer_disabled;
    (void)developer_mode;
    if (developer) {
        std::fprintf(stderr, "[rayman2] developer mode on: F1 frame inspector, "
                             "F3 view RDRAM, F4 texture replacements\n");
    }

    auto context = recompui::renderer::create_render_context(
        rdram,
        window_handle,
        presentation_mode(),
        developer);

    // Chain the draw hook, once. The guard matters if the renderer is ever
    // recreated: wrapping our own wrapper would recurse until the stack ran
    // out, on the thread that draws every frame.
    RT64::RenderHookDraw* installed = RT64::GetRenderHookDraw();
    if (installed != nullptr && installed != draw_hook_with_com) {
        recompui_draw_hook = installed;
        RT64::SetRenderHooks(RT64::GetRenderHookInit(), draw_hook_with_com, RT64::GetRenderHookDeinit());
    }

    return std::make_unique<WidescreenCullingContext>(std::move(context));
}

} // namespace rayman2
