#pragma once

#include "gameboy/ppu.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace gbb::sdl {

using SgbViewportOverlay = gameboy::Ppu::Framebuffer;

// The native 160x144 voxel scene replaces only transparent SGB viewport
// pixels. Retain opaque SNES border artwork above it as a sparse RGBA layer.
inline bool build_sgb_viewport_overlay(
    const std::vector<std::uint32_t>& colorized_border,
    const gameboy::Ppu::SgbViewportMask& opaque,
    SgbViewportOverlay& overlay) noexcept {
    overlay.fill(0);
    if (colorized_border.size() !=
        gameboy::Ppu::sgb_border_width * gameboy::Ppu::sgb_border_height) {
        return false;
    }
    constexpr auto viewport_x = (gameboy::Ppu::sgb_border_width -
                                 gameboy::Ppu::screen_width) / 2;
    constexpr auto viewport_y = (gameboy::Ppu::sgb_border_height -
                                 gameboy::Ppu::screen_height) / 2;
    bool visible = false;
    for (std::size_t y = 0; y < gameboy::Ppu::screen_height; ++y) {
        for (std::size_t x = 0; x < gameboy::Ppu::screen_width; ++x) {
            const auto index = y * gameboy::Ppu::screen_width + x;
            if (opaque[index] == 0) continue;
            visible = true;
            overlay[index] = colorized_border[
                (y + viewport_y) * gameboy::Ppu::sgb_border_width +
                x + viewport_x];
        }
    }
    return visible;
}

} // namespace gbb::sdl
