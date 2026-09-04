#include "input_configuration.hpp"

#include <array>
#include <sstream>

namespace gbb::sdl {

void begin_binding_configuration(
    InputBindings& bindings, InputBindings& backup,
    std::optional<BindingConfiguration>& configuring,
    const BindingDevice device) {
    backup = bindings;
    if (device == BindingDevice::keyboard) {
        for (auto& keys : bindings.keys) keys.fill(SDLK_UNKNOWN);
    }
    configuring = BindingConfiguration{device, 0, 0};
}

ControlsAction show_controls_dialog(SDL_Window* window,
                                    const InputBindings& bindings) {
    std::ostringstream message;
    message << "Current controls:\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        message << button_names[index] << ": "
                << SDL_GetKeyName(bindings.keys[index][0]);
        if (bindings.keys[index][1] != SDLK_UNKNOWN) {
            message << " or " << SDL_GetKeyName(bindings.keys[index][1]);
        }
        message << " / "
                << SDL_GetGamepadStringForButton(
                       bindings.gamepad_buttons[index])
                << '\n';
    }
    message << "\nChoose which controls to configure. Keyboard setup asks "
               "for a primary and optional secondary key; press Space to "
               "skip a secondary key.";

    constexpr std::array<SDL_MessageBoxButtonData, 4> buttons{{
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {0, 1, "Keyboard"},
        {0, 2, "Gamepad"},
        {0, 3, "Restore defaults"},
    }};
    const auto text = message.str();
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Configure controls", text.c_str(),
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = 0;
    if (!SDL_ShowMessageBox(&box, &selection)) return ControlsAction::cancel;
    switch (selection) {
    case 1: return ControlsAction::keyboard;
    case 2: return ControlsAction::gamepad;
    case 3: return ControlsAction::reset;
    default: return ControlsAction::cancel;
    }
}

} // namespace gbb::sdl
