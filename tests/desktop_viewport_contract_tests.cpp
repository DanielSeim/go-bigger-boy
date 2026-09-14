#include "desktop_viewport.hpp"

#include "gameboy/cartridge.hpp"
#include "gameboy/hardware_model.hpp"
#include "gameboy/memory_bus.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

gameboy::MemoryBus test_bus(const bool cgb = false) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    rom[0x143] = cgb ? 0x80 : 0x00;
    rom[0x147] = 0x00;
    rom[0x148] = 0x00;
    rom[0x149] = 0x00;
    return gameboy::MemoryBus{gameboy::Cartridge{std::move(rom)}};
}

void test_unsigned_tile_map_and_scroll() {
    auto bus = test_bus();
    bus.initialize_post_boot();
    bus.write8(0xFF40, 0x91); // LCD on, BG on, unsigned tile data, $9800 map.
    bus.write8(0xFF47, 0xE4); // Identity DMG palette mapping.
    bus.write8(0xFF42, 23);
    bus.write8(0xFF43, 197);
    bus.debug_write_vram(0, 0x1800, 0);
    bus.debug_write_vram(0, 0x0000, 0x01); // Tile 0: first pixel is color 1.

    const auto map = gbb::sdl::render_desktop_background_map(
        bus, gameboy::display_palettes[0]);
    check(map.scroll_x == 197 && map.scroll_y == 23,
          "reports the live scroll registers");
    check(map.pixels[0] == gameboy::display_palettes[0].colors[1],
          "renders unsigned tile data with the DMG palette");
    check(map.pixels[1] == gameboy::display_palettes[0].colors[0],
          "renders adjacent tile pixels in display order");
    const auto viewport = gbb::sdl::render_desktop_viewport(
        bus, gameboy::display_palettes[0]);
    check(viewport.pixels[gbb::sdl::DesktopBackgroundMap::visible_origin_y * 256 +
                            gbb::sdl::DesktopBackgroundMap::visible_origin_x] ==
              map.pixels[23 * 256 + 197],
          "centers the calculated view on the live scroll position");
}

void test_signed_tile_map_and_flips() {
    auto bus = test_bus();
    bus.initialize_post_boot();
    bus.write8(0xFF40, 0x81); // LCD on, BG on, signed tile data, $9800 map.
    bus.write8(0xFF47, 0xE4);
    bus.debug_write_vram(0, 0x1800, 0xFF); // Signed tile number -1.
    bus.debug_write_vram(0, 0x0FF0, 0x01);
    bus.debug_write_vram(0, 0x0FF0 + 1, 0x00);

    const auto map = gbb::sdl::render_desktop_background_map(
        bus, gameboy::display_palettes[0]);
    check(map.pixels[0] == gameboy::display_palettes[0].colors[1],
          "uses the signed tile-data address range");

    // Tilemap attributes are present only in CGB VRAM bank 1.
    auto cgb_bus = test_bus(true);
    cgb_bus.initialize_post_boot(gameboy::HardwareModel::cgb);
    cgb_bus.write8(0xFF68, 0x02);
    cgb_bus.write8(0xFF69, 0x1F);
    cgb_bus.write8(0xFF68, 0x03);
    cgb_bus.write8(0xFF69, 0x00);
    check(cgb_bus.debug_read_cgb_bg_palette(2) == 0x1F,
          "writes the CGB background palette used by the map");
    cgb_bus.write8(0xFF40, 0x99);
    cgb_bus.debug_write_vram(0, 0x1C00, 0);
    cgb_bus.debug_write_vram(1, 0x1C00, 0x60);
    cgb_bus.debug_write_vram(0, 0x0000, 0x80);
    const auto flipped = gbb::sdl::render_desktop_background_map(
        cgb_bus, gameboy::display_palettes[0]);
    check(flipped.pixels[7 * 256 + 7] == 0xFFFF0000,
          "honors tilemap flip attributes when reconstructing the map");
}

} // namespace

int main() {
    test_unsigned_tile_map_and_scroll();
    test_signed_tile_map_and_flips();
    return failures == 0 ? 0 : 1;
}
