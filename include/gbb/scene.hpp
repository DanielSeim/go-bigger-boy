#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gbb {

// A tile-map layer is deliberately expressed in hardware-neutral terms. A
// future core can provide a different map size or omit the layer entirely,
// while the GB adapter can expose its native 32x32 maps without leaking PPU
// implementation types through the frontend API.
struct SceneTileLayer {
    bool enabled{};
    std::uint16_t map_address{};
    bool tile_data_unsigned{};
    std::size_t width{};
    std::size_t height{};
    std::vector<std::uint8_t> tile_ids;
    std::vector<std::uint8_t> attributes;
};

struct SceneSprite {
    // Coordinates retain the values stored in OAM. Screen coordinates are
    // therefore (oam_x - 8, oam_y - 16), matching the GB hardware convention.
    std::uint8_t oam_y{};
    std::uint8_t oam_x{};
    std::uint8_t tile{};
    std::uint8_t attributes{};
    std::int16_t screen_x{};
    std::int16_t screen_y{};
    bool visible{};
};

struct SceneSnapshot {
    std::uint64_t emulation_cycles{};
    std::size_t width{};
    std::size_t height{};
    bool cgb_mode{};
    std::uint8_t lcdc{};
    std::uint8_t scx{};
    std::uint8_t scy{};
    std::uint8_t wx{};
    std::uint8_t wy{};
    std::uint8_t bg_palette{};
    std::uint8_t object_palette_0{};
    std::uint8_t object_palette_1{};
    std::uint8_t bg_palette_index{};
    std::uint8_t object_palette_index{};
    SceneTileLayer background{};
    SceneTileLayer window{};
    // Bank-major, tile-major, row-byte layout. A renderer can address a tile
    // with bank * tile_bank_stride + tile * tile_size_bytes.
    std::size_t tile_size_bytes{};
    std::size_t tile_count{};
    std::size_t tile_banks{};
    std::size_t tile_bank_stride{};
    std::vector<std::uint8_t> tile_data;
    std::vector<std::uint8_t> cgb_bg_palette;
    std::vector<std::uint8_t> cgb_object_palette;
    std::vector<SceneSprite> sprites;
};

} // namespace gbb
