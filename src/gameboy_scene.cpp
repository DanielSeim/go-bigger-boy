#include "gbb/gameboy_scene.hpp"

#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"
#include "gameboy/ppu.hpp"

#include <cstddef>
#include <cstdint>

namespace gbb {

void populate_gameboy_scene_snapshot(const gameboy::Emulator& emulator,
                                     SceneSnapshot& scene) {
    const auto& bus = emulator.bus();
    scene.emulation_cycles = emulator.cpu().total_cycles();
    scene.width = gameboy::Ppu::screen_width;
    scene.height = gameboy::Ppu::screen_height;
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
