#include "sgb_overlay.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using gameboy::Ppu;
    std::vector<std::uint32_t> border(
        Ppu::sgb_border_width * Ppu::sgb_border_height, 0xFF000000);
    Ppu::SgbViewportMask opaque{};
    gbb::sdl::SgbViewportOverlay overlay{};
    constexpr auto x = (Ppu::sgb_border_width - Ppu::screen_width) / 2;
    constexpr auto y = (Ppu::sgb_border_height - Ppu::screen_height) / 2;
    border[y * Ppu::sgb_border_width + x] = 0xFFFF0000;
    border[y * Ppu::sgb_border_width + x + 1] = 0xFF00FF00;
    opaque[0] = 1;

    if (!gbb::sdl::build_sgb_viewport_overlay(border, opaque, overlay) ||
        overlay[0] != 0xFFFF0000 || overlay[1] != 0 ||
        overlay.back() != 0) {
        std::cerr << "FAIL: only opaque SGB border pixels cover the voxel viewport\n";
        return 1;
    }
    opaque[0] = 0;
    if (gbb::sdl::build_sgb_viewport_overlay(border, opaque, overlay) ||
        overlay[0] != 0) {
        std::cerr << "FAIL: a transparent SGB viewport leaves no overlay\n";
        return 1;
    }
    return 0;
}
