// RecompFrontend integration: the launcher, the config menus and input.
//
// Everything here is compiled only when RAYMAN2_ENABLE_FRONTEND is set. The
// headless build keeps its own hardcoded keyboard map in main.cpp, which exists
// so the game can be driven during bring-up without the UI in the way.
//
// THE ORDER MATTERS, and getting it wrong does not look like getting it wrong.
// recompui builds its menus from inside RT64's setup, through a render hook it
// installs before creating the application. One of those menus is the config
// modal, and recompui::config::init_modal() *throws* when no configuration has
// been loaded:
//
//     "Configurations have not been loaded. Call recompui::config::finalize()
//      first."
//
// That throw happens on the game thread, deep inside RT64 setup, behind a
// noexcept boundary. It produces no message, no crash dialogue and no non-zero
// exit. The window is created and then torn down, the setup call never returns,
// and from the outside the port looks like it hangs with no window -- which is
// exactly what it did, for ninety seconds, until the exception was found by
// probing the frontend's own menu construction one step at a time. So: create
// every tab, then finalize, and do both before recomp::start.

#include <cstdint>
#include <vector>

#include "SDL.h"

#include <filesystem>

#include "librecomp/game.hpp"

#include "recompui/config.h"
#include "recompui/recompui.h"
#include "recompinput/input_events.h"
#include "recompinput/input_mapping.h"
#include "recompinput/input_state.h"
#include "recompinput/input_types.h"
#include "recompinput/players.h"
#include "recompinput/profiles.h"

namespace {

using recompinput::GameInput;
using recompinput::InputField;

// Rayman 2 is a single-player game with one controller in port 1.
void configure_players() {
    recompinput::players::set_player_count_range(1, 1);
    recompinput::players::set_single_player_mode(true);
}

// Defaults only. Everything here is rebindable in the Controls tab, and the
// choices are stored per device by recompinput's profile system.
//
// The scheme follows the game rather than the console's button names. Rayman 2
// wants the analog stick constantly, A to jump, B to shoot, R to strafe and the
// C buttons for the camera, so those get the comfortable keys; the D-pad, which
// this game barely uses, gets the numeric keypad.
void set_default_bindings() {
    auto kb = [](GameInput input, std::vector<InputField> fields) {
        recompinput::set_default_mapping_for_keyboard(input, fields);
    };
    auto pad = [](GameInput input, std::vector<InputField> fields) {
        recompinput::set_default_mapping_for_controller(input, fields);
    };

    // Analog stick.
    kb(GameInput::Y_AXIS_POS, { InputField::keyboard(SDL_SCANCODE_W) });
    kb(GameInput::Y_AXIS_NEG, { InputField::keyboard(SDL_SCANCODE_S) });
    kb(GameInput::X_AXIS_NEG, { InputField::keyboard(SDL_SCANCODE_A) });
    kb(GameInput::X_AXIS_POS, { InputField::keyboard(SDL_SCANCODE_D) });
    pad(GameInput::Y_AXIS_POS, { InputField::controller_analog(SDL_CONTROLLER_AXIS_LEFTY, false) });
    pad(GameInput::Y_AXIS_NEG, { InputField::controller_analog(SDL_CONTROLLER_AXIS_LEFTY, true) });
    pad(GameInput::X_AXIS_NEG, { InputField::controller_analog(SDL_CONTROLLER_AXIS_LEFTX, false) });
    pad(GameInput::X_AXIS_POS, { InputField::controller_analog(SDL_CONTROLLER_AXIS_LEFTX, true) });

    // Face buttons and triggers.
    kb(GameInput::A,     { InputField::keyboard(SDL_SCANCODE_X) });
    kb(GameInput::B,     { InputField::keyboard(SDL_SCANCODE_C) });
    kb(GameInput::Z,     { InputField::keyboard(SDL_SCANCODE_LSHIFT) });
    kb(GameInput::L,     { InputField::keyboard(SDL_SCANCODE_Q) });
    kb(GameInput::R,     { InputField::keyboard(SDL_SCANCODE_E) });
    kb(GameInput::START, { InputField::keyboard(SDL_SCANCODE_RETURN) });
    pad(GameInput::A,     { InputField::controller_digital(SDL_CONTROLLER_BUTTON_A) });
    pad(GameInput::B,     { InputField::controller_digital(SDL_CONTROLLER_BUTTON_X) });
    pad(GameInput::Z,     { InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERLEFT) });
    pad(GameInput::L,     { InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) });
    pad(GameInput::R,     { InputField::controller_analog(SDL_CONTROLLER_AXIS_TRIGGERRIGHT),
                            InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) });
    pad(GameInput::START, { InputField::controller_digital(SDL_CONTROLLER_BUTTON_START) });

    // C buttons: the camera. Arrow keys, or the right stick on a pad.
    kb(GameInput::C_UP,    { InputField::keyboard(SDL_SCANCODE_UP) });
    kb(GameInput::C_DOWN,  { InputField::keyboard(SDL_SCANCODE_DOWN) });
    kb(GameInput::C_LEFT,  { InputField::keyboard(SDL_SCANCODE_LEFT) });
    kb(GameInput::C_RIGHT, { InputField::keyboard(SDL_SCANCODE_RIGHT) });
    pad(GameInput::C_UP,    { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, false) });
    pad(GameInput::C_DOWN,  { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTY, true) });
    pad(GameInput::C_LEFT,  { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, false) });
    pad(GameInput::C_RIGHT, { InputField::controller_analog(SDL_CONTROLLER_AXIS_RIGHTX, true) });

    // D-pad.
    kb(GameInput::DPAD_UP,    { InputField::keyboard(SDL_SCANCODE_KP_8) });
    kb(GameInput::DPAD_DOWN,  { InputField::keyboard(SDL_SCANCODE_KP_2) });
    kb(GameInput::DPAD_LEFT,  { InputField::keyboard(SDL_SCANCODE_KP_4) });
    kb(GameInput::DPAD_RIGHT, { InputField::keyboard(SDL_SCANCODE_KP_6) });
    pad(GameInput::DPAD_UP,    { InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_UP) });
    pad(GameInput::DPAD_DOWN,  { InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_DOWN) });
    pad(GameInput::DPAD_LEFT,  { InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_LEFT) });
    pad(GameInput::DPAD_RIGHT, { InputField::controller_digital(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) });

    // Menu navigation. Without these the menu can be opened but not driven,
    // which is worse than not being able to open it.
    kb(GameInput::TOGGLE_MENU,    { InputField::keyboard(SDL_SCANCODE_ESCAPE) });
    kb(GameInput::ACCEPT_MENU,    { InputField::keyboard(SDL_SCANCODE_RETURN) });
    kb(GameInput::BACK_MENU,      { InputField::keyboard(SDL_SCANCODE_BACKSPACE) });
    kb(GameInput::APPLY_MENU,     { InputField::keyboard(SDL_SCANCODE_F) });
    kb(GameInput::TAB_LEFT_MENU,  { InputField::keyboard(SDL_SCANCODE_Q) });
    kb(GameInput::TAB_RIGHT_MENU, { InputField::keyboard(SDL_SCANCODE_E) });
    // Back -- the View/Select button on an Xbox pad -- opens the frontend menu,
    // NOT Start. Start belongs to the game: Rayman 2 pauses with it, and a pad
    // that cannot reach the game's own pause menu is a pad that cannot play the
    // game. Binding both to Start, as this originally did, meant the frontend
    // swallowed every press.
    pad(GameInput::TOGGLE_MENU,    { InputField::controller_digital(SDL_CONTROLLER_BUTTON_BACK) });
    pad(GameInput::ACCEPT_MENU,    { InputField::controller_digital(SDL_CONTROLLER_BUTTON_A) });
    pad(GameInput::BACK_MENU,      { InputField::controller_digital(SDL_CONTROLLER_BUTTON_B) });
    pad(GameInput::APPLY_MENU,     { InputField::controller_digital(SDL_CONTROLLER_BUTTON_Y) });
    pad(GameInput::TAB_LEFT_MENU,  { InputField::controller_digital(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) });
    pad(GameInput::TAB_RIGHT_MENU, { InputField::controller_digital(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) });
}

} // namespace

namespace {

// Where the frontend keeps key bindings, under the per-user config directory
// main.cpp registers. The name matches the one recompui itself loads from in
// config::finalize(), so the two agree on a single file.
std::filesystem::path controls_config_path() {
    return recomp::get_config_path() / "controls.json";
}

} // namespace

// src/cheats.cpp
namespace rayman2::cheats { void create_tab(); }

namespace rayman2 {

// Called from main() before recomp::start(). See the note at the top of this
// file for why "before" is not negotiable.
void frontend_init() {
    configure_players();
    set_default_bindings();

    // Create the single-player keyboard and controller profiles and assign
    // them to player 1. Nothing else does this -- config::finalize() loads a
    // saved controls.json if one exists, but with no profiles to load into the
    // bindings simply come up empty, which is what the Controls tab showed the
    // first time: every N64 input listed, every slot blank. The defaults set
    // above are applied as each profile is created, so this has to come after
    // them and before finalize().
    recompinput::profiles::initialize_input_bindings();

    // The prefab tabs. Rumble is offered because port 1 is a controller; gyro
    // and mouse aim are not, because this game has no use for either.
    recompui::config::GeneralTabOptions general{};
    general.has_rumble_strength   = true;
    general.has_gyro_sensitivity  = false;
    general.has_mouse_sensitivity = false;
    recompui::config::create_general_tab(general);
    recompui::config::create_graphics_tab();
    recompui::config::create_sound_tab();
    recompui::config::create_controls_tab();
    rayman2::cheats::create_tab();
    recompui::config::create_mods_tab();

    // Loads the config files from disk. Everything above must already exist.
    recompui::config::finalize();

    // Write the bindings out if there are none on disk yet.
    //
    // The frontend only saves controls from the Controls tab's close handler,
    // so a fresh install has no controls.json at all and a player who rebinds
    // and then dismisses the whole menu with Escape -- rather than switching
    // tabs -- loses the change. Writing the defaults here means the file always
    // exists, and frontend_shutdown() below writes it again on the way out so
    // that whatever is in effect when the port closes is what comes back.
    const std::filesystem::path controls = controls_config_path();
    std::error_code ec;
    if (!std::filesystem::exists(controls, ec)) {
        recompinput::profiles::save_controls_config(controls);
    }
}

// Persist the bindings. Called after recomp::start returns.
void frontend_shutdown() {
    recompinput::profiles::save_controls_config(controls_config_path());
}

// The port's input callbacks, answered by recompinput so that bindings,
// gamepads and hot-plugging behave as they do in the sibling ports.
void frontend_poll_input() {
    recompinput::poll_inputs();
}

bool frontend_get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    return recompinput::profiles::get_n64_input(controller_num, buttons, x, y);
}

void frontend_set_rumble(int controller_num, bool on) {
    recompinput::set_rumble(controller_num, on);
}

// Replaces the bare SDL_PollEvent loop. recompinput owns the event pump because
// it has to see every event to drive menu navigation, controller hot-plugging
// and binding capture, and it forwards to recompui what the UI needs.
void frontend_handle_events() {
    recompinput::handle_events();
    recompinput::update_rumble();
}

} // namespace rayman2
