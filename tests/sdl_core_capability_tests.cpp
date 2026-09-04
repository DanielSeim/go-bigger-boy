#include "core_capability.hpp"

#include <iostream>

int main() {
    const auto tools = gbb::CoreCapability::debugger |
                       gbb::CoreCapability::sprite_editor;
    if (!gbb::sdl::supports(tools, gbb::CoreCapability::debugger) ||
        !gbb::sdl::supports(tools, gbb::CoreCapability::sprite_editor) ||
        gbb::sdl::supports(tools, gbb::CoreCapability::link_cable) ||
        gbb::sdl::supports(nullptr, gbb::CoreCapability::debugger)) {
        std::cerr << "core capability gating regression\n";
        return 1;
    }
    return 0;
}
