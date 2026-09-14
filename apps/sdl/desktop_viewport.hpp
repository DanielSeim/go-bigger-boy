#pragma once

#include "gameboy/display_palette.hpp"
#include "gameboy/memory_bus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace gbb::sdl {

struct DesktopBackgroundMap {
    static constexpr std::size_t width = 256;
    static constexpr std::size_t height = 256;
    static constexpr unsigned visible_origin_x = 48;
    static constexpr unsigned visible_origin_y = 56;
    using Pixels = std::array<std::uint32_t, width * height>;

    Pixels pixels{};
    std::uint8_t scroll_x{};
    std::uint8_t scroll_y{};
};

namespace detail {

[[nodiscard]] inline std::uint32_t rgb555_color(const std::uint8_t low,
                                                const std::uint8_t high) noexcept {
    const auto value = static_cast<std::uint16_t>(
        low | (static_cast<std::uint16_t>(high) << 8U));
    const auto expand = [](const unsigned component) {
        return (component << 3U) | (component >> 2U);
    };
    return UINT32_C(0xFF000000) | (expand(value & 0x1FU) << 16U) |
           (expand((value >> 5U) & 0x1FU) << 8U) |
           expand((value >> 10U) & 0x1FU);
}

[[nodiscard]] inline std::uint32_t cgb_background_color(
    const gameboy::MemoryBus& bus, const std::uint8_t palette,
    const std::uint8_t color) noexcept {
    const auto offset = static_cast<std::uint8_t>(palette * 8U + color * 2U);
    return rgb555_color(bus.debug_read_cgb_bg_palette(offset),
                        bus.debug_read_cgb_bg_palette(
                            static_cast<std::uint8_t>(offset + 1U)));
}

} // namespace detail

// Reconstruct the complete 32x32 Game Boy background tilemap. This is a
// debugger view of the scene data in VRAM, not a second emulation path: it
// deliberately shows all 256x256 background pixels and leaves the live
// 160x144 output as the authoritative rasterized result.
[[nodiscard]] inline DesktopBackgroundMap render_desktop_background_map(
    const gameboy::MemoryBus& bus,
    const gameboy::DisplayPalette& palette) noexcept {
    DesktopBackgroundMap result;
    result.scroll_x = bus.read8(0xFF43);
    result.scroll_y = bus.read8(0xFF42);
    const auto lcdc = bus.read8(0xFF40);
    const auto map_base = (lcdc & 0x08U) != 0 ? 0x1C00U : 0x1800U;
    const auto cgb = bus.cgb_mode();

    for (unsigned map_y = 0; map_y < 32; ++map_y) {
        for (unsigned map_x = 0; map_x < 32; ++map_x) {
            const auto map_offset = static_cast<std::uint16_t>(
                map_base + map_y * 32U + map_x);
            const auto tile = bus.debug_read_vram(0, map_offset);
            const auto attributes = cgb ? bus.debug_read_vram(1, map_offset)
                                        : std::uint8_t{0};
            const auto tile_offset = (lcdc & 0x10U) != 0
                                         ? static_cast<unsigned>(tile) * 16U
                                         : static_cast<unsigned>(
                                               0x1000 +
                                               static_cast<std::int8_t>(tile) *
                                                   16);
            const auto bank = (attributes & 0x08U) != 0 ? 1U : 0U;

            for (unsigned tile_y = 0; tile_y < 8; ++tile_y) {
                const auto source_y = (attributes & 0x40U) != 0
                                           ? 7U - tile_y
                                           : tile_y;
                const auto low = bus.debug_read_vram(
                    static_cast<std::uint8_t>(bank),
                    static_cast<std::uint16_t>(tile_offset + source_y * 2U));
                const auto high = bus.debug_read_vram(
                    static_cast<std::uint8_t>(bank),
                    static_cast<std::uint16_t>(tile_offset + source_y * 2U + 1U));
                for (unsigned tile_x = 0; tile_x < 8; ++tile_x) {
                    const auto source_x = (attributes & 0x20U) != 0
                                              ? tile_x
                                              : 7U - tile_x;
                    const auto bit = (attributes & 0x20U) != 0
                                         ? source_x
                                         : 7U - source_x;
                    const auto color = static_cast<std::uint8_t>(
                        ((low >> bit) & 1U) | (((high >> bit) & 1U) << 1U));
                    const auto pixel = static_cast<std::size_t>(
                        (map_y * 8U + tile_y) * DesktopBackgroundMap::width +
                        map_x * 8U + tile_x);
                    result.pixels[pixel] = cgb
                                                ? detail::cgb_background_color(
                                                      bus, attributes & 0x07U,
                                                      color)
                                                : palette.colors[(bus.read8(0xFF47) >>
                                                                  (color * 2U)) &
                                                                 0x03U];
                }
            }
        }
    }
    return result;
}

// Reframe the raw tilemap around the current hardware scroll position. The
// live 160x144 viewport is placed at (48,56), leaving the surrounding pixels
// visible on every side while retaining the Game Boy's modulo-256 wrapping.
[[nodiscard]] inline DesktopBackgroundMap render_desktop_viewport(
    const gameboy::MemoryBus& bus,
    const gameboy::DisplayPalette& palette) noexcept {
    // Keep the two 256 KiB frame buffers off the relatively small Windows
    // test/application stack. The returned map retains the same value-type
    // API, while the raw map is only an implementation temporary.
    const auto raw = std::make_unique<DesktopBackgroundMap>(
        render_desktop_background_map(bus, palette));
    auto result = std::make_unique<DesktopBackgroundMap>();
    result->scroll_x = raw->scroll_x;
    result->scroll_y = raw->scroll_y;
    for (unsigned y = 0; y < DesktopBackgroundMap::height; ++y) {
        for (unsigned x = 0; x < DesktopBackgroundMap::width; ++x) {
            const auto source_x =
                (static_cast<unsigned>(raw->scroll_x) + x + 256U -
                 DesktopBackgroundMap::visible_origin_x) & 0xFFU;
            const auto source_y =
                (static_cast<unsigned>(raw->scroll_y) + y + 256U -
                 DesktopBackgroundMap::visible_origin_y) & 0xFFU;
            result->pixels[static_cast<std::size_t>(y) *
                               DesktopBackgroundMap::width + x] =
                raw->pixels[static_cast<std::size_t>(source_y) *
                                DesktopBackgroundMap::width + source_x];
        }
    }
    return *result;
}

} // namespace gbb::sdl
