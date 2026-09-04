#pragma once

#include "settings_model.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <optional>

namespace gbb::sdl {

enum class BindingDevice { keyboard, gamepad };

struct BindingConfiguration {
    BindingDevice device{};
    std::size_t index{};
    std::size_t slot{};
};

enum class ControlsAction { cancel, keyboard, gamepad, reset };

void begin_binding_configuration(
    InputBindings& bindings, InputBindings& backup,
    std::optional<BindingConfiguration>& configuring, BindingDevice device);

[[nodiscard]] ControlsAction show_controls_dialog(
    SDL_Window* window, const InputBindings& bindings);

} // namespace gbb::sdl
