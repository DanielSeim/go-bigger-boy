#include "input_configuration.hpp"

#include <cassert>

int main() {
    InputBindings bindings;
    InputBindings backup = bindings;
    std::optional<gbb::sdl::BindingConfiguration> configuring;

    bindings.keys[0][0] = SDLK_A;
    gbb::sdl::begin_binding_configuration(
        bindings, backup, configuring, gbb::sdl::BindingDevice::keyboard);
    assert(configuring.has_value());
    assert(configuring->device == gbb::sdl::BindingDevice::keyboard);
    assert(configuring->index == 0);
    assert(configuring->slot == 0);
    assert(backup.keys[0][0] == SDLK_A);
    assert(bindings.keys[0][0] == SDLK_UNKNOWN);
    assert(bindings.keys[7][1] == SDLK_UNKNOWN);

    gbb::sdl::begin_binding_configuration(
        bindings, backup, configuring, gbb::sdl::BindingDevice::gamepad);
    assert(configuring->device == gbb::sdl::BindingDevice::gamepad);
    assert(bindings.keys[0][0] == SDLK_UNKNOWN);
    return 0;
}
