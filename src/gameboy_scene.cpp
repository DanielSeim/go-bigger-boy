#include "gbb/gameboy_scene.hpp"

#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"
#include "gameboy/ppu.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace gbb {
namespace {

void append_visible_tile_cells(SceneSnapshot& scene,
                               const SceneTileLayer& layer,
                               const SceneTileSource source,
                               const int origin_x,
                               const int origin_y,
                               const std::size_t first_map_x,
                               const std::size_t first_map_y) {
    constexpr int tile_size = 8;
    constexpr int viewport_width = gameboy::Ppu::screen_width;
    constexpr int viewport_height = gameboy::Ppu::screen_height;
    if (!layer.enabled || layer.width == 0 || layer.height == 0) return;

    const auto append_cell = [&](const int column, const int row,
                                 const int screen_x, const int screen_y) {
        const auto right = std::min(screen_x + tile_size, viewport_width);
        const auto bottom = std::min(screen_y + tile_size, viewport_height);
        const auto left = std::max(screen_x, 0);
        const auto top = std::max(screen_y, 0);
        if (right <= left || bottom <= top) return;

        const auto map_x = (first_map_x + static_cast<std::size_t>(column)) %
                           layer.width;
        const auto map_y = (first_map_y + static_cast<std::size_t>(row)) %
                           layer.height;
        const auto map_index = map_y * layer.width + map_x;
        const auto tile_id = layer.tile_ids[map_index];
        const auto attributes = layer.attributes[map_index];
        const auto tile_bank = scene.cgb_mode
                                   ? static_cast<std::size_t>((attributes >> 3U) & 1U)
                                   : 0U;
        const auto signed_tile = static_cast<int>(
            static_cast<std::int8_t>(tile_id));
        const auto tile_data_index = layer.tile_data_unsigned
                                         ? static_cast<std::size_t>(tile_id)
                                         : static_cast<std::size_t>(0x100 + signed_tile);

        SceneVisibleTileCell cell{};
        cell.source = source;
        cell.screen_x = static_cast<std::int16_t>(screen_x);
        cell.screen_y = static_cast<std::int16_t>(screen_y);
        cell.visible_width = static_cast<std::uint8_t>(right - left);
        cell.visible_height = static_cast<std::uint8_t>(bottom - top);
        cell.map_x = static_cast<std::uint8_t>(map_x);
        cell.map_y = static_cast<std::uint8_t>(map_y);
        cell.map_address = static_cast<std::uint16_t>(
            layer.map_address + map_y * layer.width + map_x);
        cell.tile_id = tile_id;
        cell.attributes = attributes;
        cell.tile_data_index = static_cast<std::uint16_t>(tile_data_index);
        cell.tile_bank = static_cast<std::uint8_t>(tile_bank);
        cell.palette = scene.cgb_mode
                           ? static_cast<std::uint8_t>(attributes & 0x07U)
                           : 0;

        const auto tile_offset = tile_bank * scene.tile_bank_stride +
                                 tile_data_index * scene.tile_size_bytes;
        if (tile_data_index < scene.tile_count &&
            tile_offset + 1 < scene.tile_data.size()) {
            const auto x_flip = (attributes & 0x20U) != 0;
            const auto y_flip = (attributes & 0x40U) != 0;
            for (unsigned displayed_y = 0; displayed_y < 8; ++displayed_y) {
                const auto source_y = y_flip ? 7U - displayed_y : displayed_y;
                const auto low = scene.tile_data[tile_offset + source_y * 2U];
                const auto high = scene.tile_data[tile_offset + source_y * 2U + 1U];
                for (unsigned displayed_x = 0; displayed_x < 8; ++displayed_x) {
                    const auto source_x = x_flip ? 7U - displayed_x : displayed_x;
                    const auto bit = 7U - source_x;
                    const auto color = static_cast<unsigned>(
                        ((low >> bit) & 0x01U) |
                        (((high >> bit) & 0x01U) << 1U));
                    if (color != 0) {
                        cell.opaque_mask[displayed_y] |=
                            static_cast<std::uint8_t>(1U << displayed_x);
                    }
                }
            }
        }
        scene.visible_tile_cells.push_back(cell);
    };

    for (int row = 0;; ++row) {
        const auto screen_y = origin_y + row * tile_size;
        if (screen_y >= viewport_height) break;
        for (int column = 0;; ++column) {
            const auto screen_x = origin_x + column * tile_size;
            if (screen_x >= viewport_width) break;
            append_cell(column, row, screen_x, screen_y);
        }
    }
}

} // namespace

void populate_gameboy_scene_snapshot(const gameboy::Emulator& emulator,
                                     SceneSnapshot& scene) {
    // The adapter refreshes the whole snapshot on demand. Clear optional
    // contributions so a future extension cannot accidentally retain stale
    // layers after a core state transition.
    scene.layers.clear();
    scene.visible_tile_cells.clear();
    scene.producer_id = "gameboy";
    const auto& bus = emulator.bus();
    scene.emulation_cycles = emulator.cpu().total_cycles();
    const auto sgb_model = emulator.hardware_model() == gameboy::HardwareModel::sgb ||
                           emulator.hardware_model() == gameboy::HardwareModel::sgb2;
    scene.width = sgb_model ? gameboy::Ppu::sgb_border_width
                            : gameboy::Ppu::screen_width;
    scene.height = sgb_model ? gameboy::Ppu::sgb_border_height
                             : gameboy::Ppu::screen_height;
    scene.cgb_mode = bus.cgb_mode();
    scene.lcdc = bus.read8(0xFF40);
    scene.scx = bus.read8(0xFF43);
    scene.scy = bus.read8(0xFF42);
    scene.wx = bus.read8(0xFF4B);
    scene.wy = bus.read8(0xFF4A);
    scene.bg_palette = bus.read8(0xFF47);
    scene.object_palette_0 = bus.read8(0xFF48);
    scene.object_palette_1 = bus.read8(0xFF49);
    scene.bg_palette_index = bus.read8(0xFF68);
    scene.object_palette_index = bus.read8(0xFF6A);

    const auto fill_layer = [&](SceneTileLayer& layer,
                                const std::uint16_t address,
                                const bool enabled) {
        layer.enabled = enabled;
        layer.map_address = address;
        layer.width = 32;
        layer.height = 32;
        layer.tile_data_unsigned = (scene.lcdc & 0x10U) != 0;
        layer.tile_ids.resize(layer.width * layer.height);
        layer.attributes.resize(layer.width * layer.height);
        const auto offset = static_cast<std::uint16_t>(address - 0x8000);
        for (std::size_t index = 0; index < layer.tile_ids.size(); ++index) {
            const auto map_offset = static_cast<std::uint16_t>(
                offset + static_cast<std::uint16_t>(index));
            layer.tile_ids[index] = bus.debug_read_vram(0, map_offset);
            layer.attributes[index] = scene.cgb_mode
                                          ? bus.debug_read_vram(1, map_offset)
                                          : 0;
        }
    };
    const auto bg_map = (scene.lcdc & 0x08U) != 0 ? 0x9C00 : 0x9800;
    const auto window_map = (scene.lcdc & 0x40U) != 0 ? 0x9C00 : 0x9800;
    fill_layer(scene.background, bg_map, (scene.lcdc & 0x01U) != 0);
    fill_layer(scene.window, window_map, (scene.lcdc & 0x20U) != 0);

    scene.tile_size_bytes = 16;
    scene.tile_count = 384;
    scene.tile_banks = scene.cgb_mode ? 2 : 1;
    scene.tile_bank_stride = scene.tile_count * scene.tile_size_bytes;
    scene.tile_data.resize(scene.tile_banks * scene.tile_bank_stride);
    for (std::size_t bank = 0; bank < scene.tile_banks; ++bank) {
        for (std::size_t byte = 0; byte < scene.tile_bank_stride; ++byte) {
            scene.tile_data[bank * scene.tile_bank_stride + byte] =
                bus.debug_read_vram(static_cast<std::uint8_t>(bank),
                                    static_cast<std::uint16_t>(byte));
        }
    }
    scene.cgb_bg_palette.resize(0x40);
    scene.cgb_object_palette.resize(0x40);
    for (std::size_t index = 0; index < scene.cgb_bg_palette.size(); ++index) {
        scene.cgb_bg_palette[index] = bus.debug_read_cgb_bg_palette(
            static_cast<std::uint8_t>(index));
        scene.cgb_object_palette[index] = bus.debug_read_cgb_object_palette(
            static_cast<std::uint8_t>(index));
    }

    // Export the visible native viewport as map-aware cells. Background cells
    // and overlapping Window cells are intentionally retained together so a
    // future object detector can apply layer ordering and partial clipping
    // without reconstructing provenance from the final framebuffer.
    scene.visible_tile_cells.reserve(21U * 19U * 2U);
    const auto background_origin_x = -static_cast<int>(scene.scx & 0x07U);
    const auto background_origin_y = -static_cast<int>(scene.scy & 0x07U);
    append_visible_tile_cells(
        scene, scene.background, SceneTileSource::background,
        background_origin_x, background_origin_y, scene.scx / 8U,
        scene.scy / 8U);
    if (scene.window.enabled) {
        append_visible_tile_cells(
            scene, scene.window, SceneTileSource::window,
            static_cast<int>(scene.wx) - 7, scene.wy, 0, 0);
    }

    scene.sprites.resize(40);
    const auto object_height = (scene.lcdc & 0x04U) != 0 ? 16 : 8;
    for (std::size_t index = 0; index < scene.sprites.size(); ++index) {
        const auto offset = static_cast<std::uint8_t>(index * 4);
        auto& sprite = scene.sprites[index];
        sprite.oam_y = bus.debug_read_oam(offset);
        sprite.oam_x = bus.debug_read_oam(
            static_cast<std::uint8_t>(offset + 1));
        sprite.tile = bus.debug_read_oam(static_cast<std::uint8_t>(offset + 2));
        sprite.attributes = bus.debug_read_oam(
            static_cast<std::uint8_t>(offset + 3));
        sprite.screen_x = static_cast<std::int16_t>(sprite.oam_x) - 8;
        sprite.screen_y = static_cast<std::int16_t>(sprite.oam_y) - 16;
        sprite.visible = (scene.lcdc & 0x02U) != 0 &&
                         sprite.oam_x != 0 && sprite.oam_y != 0 &&
                         sprite.screen_x < static_cast<std::int16_t>(scene.width) &&
                         sprite.screen_x + 8 > 0 &&
                         sprite.screen_y < static_cast<std::int16_t>(scene.height) &&
                         sprite.screen_y + object_height > 0;
    }
}

} // namespace gbb
