#include "input_mapping.hpp"

#include <cassert>

int main() {
    InputBindings bindings;
    assert(gbb::sdl::keyboard_button(bindings, SDLK_RIGHT) ==
           gameboy::Button::right);
    assert(gbb::sdl::keyboard_button(bindings, SDLK_X) ==
           gameboy::Button::a);
    assert(!gbb::sdl::keyboard_button(bindings, SDLK_UNKNOWN));
    assert(gbb::sdl::local_link_keyboard_button(SDLK_W) ==
           gameboy::Button::up);
    assert(gbb::sdl::local_link_keyboard_button(SDLK_J) ==
           gameboy::Button::a);
    assert(gbb::sdl::gamepad_button(
               bindings, static_cast<Uint8>(SDL_GAMEPAD_BUTTON_START)) ==
           gameboy::Button::start);
    const std::array<SDL_JoystickID, 4> gamepads{11, 12, 13, 14};
    assert(gbb::sdl::sgb_gamepad_player(11, gamepads, false) == 0);
    assert(!gbb::sdl::sgb_gamepad_player(12, gamepads, false));
    assert(gbb::sdl::sgb_gamepad_player(12, gamepads, true) == 1);
    assert(gbb::sdl::sgb_gamepad_player(13, gamepads, true) == 2);
    assert(gbb::sdl::sgb_gamepad_player(14, gamepads, true) == 3);
    assert(!gbb::sdl::sgb_gamepad_player(15, gamepads, true));
    assert(gbb::sdl::core_input_id(gameboy::Button::right) ==
           gbb::InputId::right);
    assert(gbb::sdl::core_input_id(gameboy::Button::select) ==
           gbb::InputId::select);
    assert(gbb::sdl::shortcut_pressed(bindings, shortcut_save_state, SDLK_F5));
    assert(gbb::sdl::reserved_gameplay_key(bindings, SDLK_ESCAPE));
    assert(gbb::sdl::reserved_gameplay_key(bindings, SDLK_F5));
    assert(!gbb::sdl::reserved_gameplay_key(bindings, SDLK_H));
    return 0;
}
