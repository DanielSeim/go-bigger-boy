#pragma once

#include "gameboy/emulator.hpp"
#include "gbb/core.hpp"
#include "settings_model.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <optional>

namespace gbb::sdl {

inline constexpr std::array<gameboy::Button, 8> button_order{
    gameboy::Button::right, gameboy::Button::left, gameboy::Button::up,
    gameboy::Button::down, gameboy::Button::a, gameboy::Button::b,
    gameboy::Button::select, gameboy::Button::start,
};

[[nodiscard]] std::optional<gameboy::Button>
keyboard_button(const InputBindings& bindings, SDL_Keycode key) noexcept;
[[nodiscard]] std::optional<gameboy::Button>
local_link_keyboard_button(SDL_Keycode key) noexcept;
[[nodiscard]] std::optional<gameboy::Button>
gamepad_button(const InputBindings& bindings, Uint8 button) noexcept;
[[nodiscard]] gbb::InputId core_input_id(gameboy::Button button) noexcept;
[[nodiscard]] bool shortcut_pressed(const InputBindings& bindings,
                                    std::size_t shortcut,
                                    SDL_Keycode key) noexcept;
[[nodiscard]] bool reserved_gameplay_key(const InputBindings& bindings,
                                         SDL_Keycode key) noexcept;
void release_all_buttons(gameboy::Emulator& emulator) noexcept;
void release_all_buttons(gbb::EmulatorCore& core) noexcept;

} // namespace gbb::sdl
