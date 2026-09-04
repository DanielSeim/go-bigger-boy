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
    const gbb::sdl::CoreServices adapter{nullptr, nullptr};
    if (adapter.get(gbb::CoreCapability::debugger) != nullptr ||
        adapter.debugger() != nullptr || adapter.sprite_editor() != nullptr ||
        adapter.cheats() != nullptr || adapter.link_cable() != nullptr) {
        std::cerr << "null core must not expose Game Boy tools\n";
        return 1;
    }
    return 0;
}
