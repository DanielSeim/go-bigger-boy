#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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

enum class SceneTileSource : std::uint8_t {
    background,
    window,
};

// A visible 8x8 tile cell in the native Game Boy 160x144 viewport. The cell
// may be clipped at the viewport edge when SCX/SCY or WX/WY is not tile aligned.
// `opaque_mask` is in displayed orientation, while tile_id/attributes preserve
// the source map entry. Background and Window cells are both exported when they
// overlap so consumers can apply the hardware layer ordering themselves.
struct SceneVisibleTileCell {
    SceneTileSource source{SceneTileSource::background};
    std::int16_t screen_x{};
    std::int16_t screen_y{};
    std::uint8_t visible_width{};
    std::uint8_t visible_height{};
    std::uint8_t map_x{};
    std::uint8_t map_y{};
    std::uint16_t map_address{};
    std::uint8_t tile_id{};
    std::uint8_t attributes{};
    std::uint16_t tile_data_index{};
    std::uint8_t tile_bank{};
    std::uint8_t palette{};
    std::array<std::uint8_t, 8> opaque_mask{};
};

// An optional, core-defined scene layer. The format identifier is deliberately
// opaque to frontends (for example, "vendor.core.tile-map.v1"). Cores
// advertise the formats they emit through CoreDescriptor. Consumers should
// render only formats they understand and safely fall back to the framebuffer
// for the rest. This prevents the common scene contract from accumulating
// another core's hardware registers while still allowing richer visualizers
// and diagnostics.
struct SceneLayer {
    std::string id;
    std::string format;
    std::size_t width{};
    std::size_t height{};
    std::vector<std::uint8_t> payload;
};

struct SceneSnapshot {
    // The producer identifies the adapter that populated legacy fields and
    // opaque layers. Generic renderers may use it for capability routing;
    // unknown producers remain safe to display through the framebuffer.
    std::string producer_id;
    // The legacy fields below remain populated for the current Game Boy
    // renderer. New cores should prefer `layers` and advertise scene_layers.
    std::uint32_t schema_version{1};
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
    // Native viewport provenance used by offline voxel/object analysis. This
    // remains empty for cores that do not expose a tile-based scene.
    std::vector<SceneVisibleTileCell> visible_tile_cells;
    std::vector<SceneLayer> layers;
};

} // namespace gbb
