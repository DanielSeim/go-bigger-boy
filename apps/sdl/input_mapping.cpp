#include "input_mapping.hpp"

#include <algorithm>

namespace gbb::sdl {

std::optional<gameboy::Button> keyboard_button(
    const InputBindings& bindings, const SDL_Keycode key) noexcept {
    for (std::size_t index = 0; index < bindings.keys.size(); ++index) {
        if (key != SDLK_UNKNOWN &&
            std::find(bindings.keys[index].begin(), bindings.keys[index].end(),
                      key) != bindings.keys[index].end()) {
            return button_order[index];
        }
    }
    return std::nullopt;
}

std::optional<gameboy::Button> local_link_keyboard_button(
    const SDL_Keycode key) noexcept {
    switch (key) {
    case SDLK_D: return gameboy::Button::right;
    case SDLK_A: return gameboy::Button::left;
    case SDLK_W: return gameboy::Button::up;
    case SDLK_S: return gameboy::Button::down;
    case SDLK_J: return gameboy::Button::a;
    case SDLK_K: return gameboy::Button::b;
    case SDLK_Q: return gameboy::Button::select;
    case SDLK_E: return gameboy::Button::start;
    default: return std::nullopt;
    }
}

std::optional<gameboy::Button> gamepad_button(
    const InputBindings& bindings, const Uint8 button) noexcept {
    for (std::size_t index = 0; index < bindings.gamepad_buttons.size(); ++index) {
        if (bindings.gamepad_buttons[index] ==
            static_cast<SDL_GamepadButton>(button)) {
            return button_order[index];
        }
    }
    return std::nullopt;
}

gbb::InputId core_input_id(const gameboy::Button button) noexcept {
    switch (button) {
    case gameboy::Button::right: return gbb::InputId::right;
    case gameboy::Button::left: return gbb::InputId::left;
    case gameboy::Button::up: return gbb::InputId::up;
    case gameboy::Button::down: return gbb::InputId::down;
    case gameboy::Button::a: return gbb::InputId::a;
    case gameboy::Button::b: return gbb::InputId::b;
    case gameboy::Button::select: return gbb::InputId::select;
    case gameboy::Button::start: return gbb::InputId::start;
    }
    return gbb::InputId::a;
}

bool shortcut_pressed(const InputBindings& bindings, const std::size_t shortcut,
                      const SDL_Keycode key) noexcept {
    return shortcut < bindings.shortcuts.size() &&
           bindings.shortcuts[shortcut] != SDLK_UNKNOWN &&
           bindings.shortcuts[shortcut] == key;
}

bool reserved_gameplay_key(const InputBindings& bindings,
                           const SDL_Keycode key) noexcept {
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_SPACE:
    case SDLK_F1:
    case SDLK_F11:
        return true;
    default:
        return std::find(bindings.shortcuts.begin(), bindings.shortcuts.end(),
                         key) != bindings.shortcuts.end();
    }
}

void release_all_buttons(gameboy::Emulator& emulator) noexcept {
    for (const auto button : button_order) emulator.set_button(button, false);
}

void release_all_buttons(gbb::EmulatorCore& core) noexcept {
    for (const auto button : button_order) {
        core.set_input(core_input_id(button), false);
    }
}

} // namespace gbb::sdl
