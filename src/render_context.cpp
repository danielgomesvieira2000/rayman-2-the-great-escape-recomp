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

#include <memory>

#include "ultramodern/renderer_context.hpp"
#include "recompui/renderer.h"

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
    return recompui::renderer::create_render_context(
        rdram,
        window_handle,
        ultramodern::renderer::PresentationMode::Console,
        developer_mode);
}

} // namespace rayman2
