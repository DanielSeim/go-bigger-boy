#include "gameboy/emulator.hpp"
#include "gameboy/joypad.hpp"
#include "gameboy/ppu.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const char title[] = "SGB CONTRACT";
    for (std::size_t index = 0; title[index] != '\0'; ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    rom[0x146] = 0x03; // Super Game Boy enhanced
    rom[0x147] = 0x00; // ROM only
    rom[0x148] = 0x00; // 32 KiB
    rom[0x149] = 0x00; // no external RAM
    rom[0x100] = 0x00;
    return rom;
}

void advance_sgb_frames(gameboy::Ppu& ppu, const unsigned count) {
    static_cast<void>(ppu.write_register(0xFF40, 0x91));
    for (unsigned frame = 0; frame < count; ++frame) {
        static_cast<void>(ppu.tick(70224));
    }
}

void test_transfer_commands_and_guards() {
    gameboy::Ppu ppu;
    std::array<std::uint8_t, 16 * 7> packet{};
    packet[0] = static_cast<std::uint8_t>(0x13U << 3); // CHR_TRN

    // The transfer source is the indexed display image. Tile zero's first
    // row has one colour-1 pixel at the right edge, yielding packed value 1.
    ppu.debug_write_vram(0, 0x0000, 0x01);
    ppu.debug_write_vram(0, 0x0001, 0x00);
    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0,
          "SGB commands are ignored while SGB mode is disabled");

    ppu.set_sgb_mode(true);
    ppu.apply_sgb_command(packet, 1);
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0,
          "truncated SGB packets do not modify transfer latches");

    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0,
          "CHR_TRN transfer remains pending during its hardware delay");
    advance_sgb_frames(ppu, 2);
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0,
          "CHR_TRN transfer remains pending during the first two frames");
    advance_sgb_frames(ppu, 2);
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0,
          "CHR_TRN transfer remains pending through the fourth frame");
    advance_sgb_frames(ppu, 1);
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0x01 &&
              ppu.debug_read_sgb_border_tile(0x0001) == 0x00,
          "CHR_TRN encodes the first tile-data bank from the display");

    packet[1] = 1;
    ppu.debug_write_vram(0, 0x0000, 0x80);
    ppu.debug_write_vram(0, 0x0001, 0x00);
    ppu.apply_sgb_command(packet, packet.size());
    advance_sgb_frames(ppu, 5);
    check(ppu.debug_read_sgb_border_tile(0x1000) == 0x80 &&
              ppu.debug_read_sgb_border_tile(0x1FFF) == 0x00,
          "CHR_TRN selects the second tile-data bank");

    packet[0] = static_cast<std::uint8_t>(0x14U << 3); // PCT_TRN
    ppu.debug_write_vram(0, 0x0000, 0x01);
    ppu.debug_write_vram(0, 0x0001, 0x00);
    ppu.apply_sgb_command(packet, packet.size());
    advance_sgb_frames(ppu, 5);
    check(ppu.debug_read_sgb_border_pct(0x0000) == 0x01 &&
              ppu.debug_read_sgb_border_pct(0x0001) == 0x00 &&
              ppu.debug_read_sgb_border_pct(0x0FFF) == 0x00,
          "PCT_TRN encodes the complete border payload");
}

void test_mask_command_is_bounded() {
    gameboy::Ppu ppu;
    ppu.set_sgb_mode(true);
    std::array<std::uint8_t, 16 * 7> packet{};
    packet[0] = static_cast<std::uint8_t>(0x17U << 3); // MASK_EN
    packet[1] = 0xFE;
    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.sgb_mask_mode() == 2,
          "MASK_EN stores only the two-bit mask mode");
}

void test_multiplayer_request_and_polling() {
    gameboy::Joypad joypad;
    joypad.set_sgb_mode(true);
    std::array<std::uint8_t, 16 * 7> packet{};
    packet[0] = static_cast<std::uint8_t>(0x11 << 3); // MLT_REQ
    packet[1] = 0x01; // two players
    joypad.apply_sgb_command(packet, packet.size());
    check(joypad.sgb_player_count() == 2,
          "MLT_REQ enables two-player SGB polling");
    static_cast<void>(joypad.write(0x30));
    check((joypad.read() & 0x0F) == 0x0F,
          "SGB multiplayer polling starts with player one ID");
    static_cast<void>(joypad.write(0x10));
    static_cast<void>(joypad.write(0x30));
    check((joypad.read() & 0x0F) == 0x0E,
          "SGB multiplayer polling advances to player two");
    packet[1] = 0x03; // four players
    joypad.apply_sgb_command(packet, packet.size());
    check(joypad.sgb_player_count() == 4,
          "MLT_REQ enables four-player SGB polling");
    static_cast<void>(joypad.write(0x10));
    static_cast<void>(joypad.write(0x30));
    check((joypad.read() & 0x0F) == 0x0D,
          "SGB four-player polling advances through controller IDs");
}

void test_multiplayer_command_reaches_joypad() {
    gameboy::Emulator emulator{gameboy::Cartridge{test_rom()}};
    std::array<std::uint8_t, 16> command{};
    command[0] = static_cast<std::uint8_t>((0x11U << 3) | 1U);
    command[1] = 0x01;
    const auto write = [&](const std::uint8_t value) {
        emulator.bus().write8(0xFF00, value);
    };
    write(0x30);
    write(0x00);
    for (std::size_t bit = 0; bit < command.size() * 8; ++bit) {
        write(0x30);
        write((command[bit / 8] & (1U << (bit & 7U))) != 0 ? 0x10 : 0x20);
    }
    write(0x30);
    write(0x20);
    emulator.bus().write8(0xFF00, 0x30);
    check((emulator.bus().read8(0xFF00) & 0x0F) == 0x0F,
          "MLT_REQ packets configure the bus joypad controller ID");
    emulator.bus().write8(0xFF00, 0x10);
    emulator.bus().write8(0xFF00, 0x30);
    check((emulator.bus().read8(0xFF00) & 0x0F) == 0x0E,
          "bus joypad polling advances after P15 is released");
}

void test_sgb_first_packet_accepts_start_pulse() {
    gameboy::Joypad joypad;
    joypad.set_sgb_mode(true);
    std::array<std::uint8_t, gameboy::Joypad::sgb_packet_size> command{};
    command[0] = static_cast<std::uint8_t>((0x11U << 3) | 1U);
    command[1] = 0x01; // two players

    // A first packet starts directly with 00, unlike subsequent packets
    // which are preceded by the previous packet's 30 finish write.
    static_cast<void>(joypad.write(0x00));
    static_cast<void>(joypad.write(0x30));
    for (std::size_t bit = 0; bit < command.size() * 8; ++bit) {
        static_cast<void>(joypad.write(0x30));
        static_cast<void>(joypad.write(
            (command[bit / 8] & (1U << (bit & 7U))) != 0 ? 0x10 : 0x20));
    }
    static_cast<void>(joypad.write(0x30));
    static_cast<void>(joypad.write(0x20));

    std::array<std::uint8_t, gameboy::Joypad::sgb_packet_size *
                                  gameboy::Joypad::sgb_max_packets>
        packet{};
    std::size_t size = 0;
    check(joypad.take_sgb_packet(packet, size) && size == command.size() &&
              packet[0] == command[0] && packet[1] == command[1],
          "SGB parser accepts the first packet's direct start pulse");
}

void test_malformed_command_matrix_is_bounded() {
    gameboy::Ppu ppu;
    ppu.set_sgb_mode(true);
    std::array<std::uint8_t, 16 * 7> packet{};
    std::uint32_t state = 0x51B00B5U;
    for (unsigned command = 0; command < 32; ++command) {
        packet.fill(0);
        packet[0] = static_cast<std::uint8_t>(command << 3);
        for (std::size_t index = 1; index < packet.size(); ++index) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            packet[index] = static_cast<std::uint8_t>(state);
        }
        for (const std::size_t size : {std::size_t{0}, std::size_t{1},
                                        std::size_t{2}, std::size_t{7},
                                        packet.size()}) {
            ppu.apply_sgb_command(packet, size);
            check(ppu.sgb_mask_mode() <= 3,
                  "malformed SGB command keeps mask mode bounded");
        }
    }
}

void test_palette_command_reaches_video() {
    gameboy::Emulator emulator{gameboy::Cartridge{test_rom()}};
    std::array<std::uint8_t, 16> command{};
    command[0] = 0x01; // PAL01
    command[1] = 0x1F;
    command[4] = 0x7C;
    command[5] = 0xE0;
    command[6] = 0x03;
    command[7] = 0xFF;
    command[8] = 0x7F;
    command[9] = 0x1F;
    command[12] = 0x7C;
    command[13] = 0xE0;
    command[14] = 0x03;

    const auto write = [&](const std::uint8_t value) {
        emulator.bus().write8(0xFF00, value);
    };
    write(0x30);
    write(0x00);
    for (std::size_t bit = 0; bit < command.size() * 8; ++bit) {
        write(0x30);
        write((command[bit / 8] & (1U << (bit & 7U))) != 0 ? 0x10 : 0x20);
    }
    write(0x30);
    write(0x20);
    emulator.bus().write8(0xFF40, 0x00);
    emulator.bus().write8(0xFF40, 0x91);
    emulator.bus().tick(70224);
    check(emulator.framebuffer()[0] == 0xFFFF0000,
          "PAL01 updates the rendered SGB palette");
}

void test_palette_and_attribute_transfer_commands() {
    gameboy::Ppu ppu;
    ppu.set_sgb_mode(true);
    for (std::size_t index = 0; index < 0x1000; ++index) {
        ppu.debug_write_vram(0, static_cast<std::uint16_t>(index), 0);
    }
    for (unsigned row = 0; row < 8; ++row) {
        // One colour-1 pixel per row, moving left, produces packed entries
        // 1, 2, 4, ... 0x80 in the PAL_TRN stream.
        ppu.debug_write_vram(0, static_cast<std::uint16_t>(row * 2),
                             static_cast<std::uint8_t>(1U << row));
    }

    std::array<std::uint8_t, 16 * 7> packet{};
    packet[0] = static_cast<std::uint8_t>(0x0B << 3); // PAL_TRN
    ppu.apply_sgb_command(packet, packet.size());
    advance_sgb_frames(ppu, 5);
    check(ppu.debug_read_sgb_palette(0) == 0x0001 &&
              ppu.debug_read_sgb_palette(0x7FF) == 0x0080,
          "PAL_TRN stores all transferred RGB555 palette entries");

    packet.fill(0);
    packet[0] = static_cast<std::uint8_t>(0x0A << 3); // PAL_SET
    packet[1] = 0x01;
    packet[3] = 0x02;
    packet[5] = 0x03;
    packet[7] = 0x04;
    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_palette(1) == 0x0002 &&
              ppu.debug_read_sgb_active_palette(4) == 0x0010,
          "PAL_SET selects colors from transferred palette memory");

    ppu.debug_write_vram(0, 0x0000, 0x40);
    packet.fill(0);
    packet[0] = static_cast<std::uint8_t>(0x15 << 3); // ATTR_TRN
    ppu.apply_sgb_command(packet, packet.size());
    advance_sgb_frames(ppu, 5);
    packet.fill(0);
    packet[0] = static_cast<std::uint8_t>(0x16 << 3); // ATTR_SET
    packet[1] = 0x00;
    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_attribute(0, 0) == 1 &&
              ppu.debug_read_sgb_attribute(1, 0) == 0,
          "ATTR_TRN and ATTR_SET restore packed tile attributes");
}

void test_sgb_border_compositor() {
    gameboy::Ppu ppu;
    ppu.set_sgb_mode(true);

    // The SGB samples the rendered 2-bit screen image. Arrange source tiles
    // so CHR_TRN produces solid border tile 1, PCT_TRN produces map entry 1,
    // and palette 0 colour 1 is RGB555 red.
    for (std::size_t row = 0; row < 8; ++row) {
        ppu.debug_write_vram(0, static_cast<std::uint16_t>(0x10 + row * 2),
                             0xFF);
        ppu.debug_write_vram(0,
                             static_cast<std::uint16_t>(0x10 + row * 2 + 1),
                             0x00);
    }
    // Source tile 0 row 0 packs to map entry 1 (one colour-1 pixel at x=7).
    ppu.debug_write_vram(0, 0x0000, 0x01);
    // Source tile 2 becomes the low 2-bit half of SNES tile 1; leave source
    // tile 3 at colour zero so the resulting border pixel is SNES colour 1.
    ppu.debug_write_vram(0, 0x1802, 0x01);
    // Source tile 128 supplies PCT palette colour zero (0x7C00) and colour
    // one (0x001F) in successive rows.
    ppu.debug_write_vram(0, 0x18C8, 0x80);
    ppu.debug_write_vram(0, 0x0800, 0x00);
    ppu.debug_write_vram(0, 0x0801, 0x7C);
    ppu.debug_write_vram(0, 0x0802, 0x1F);
    ppu.debug_write_vram(0, 0x0803, 0x00);
    std::array<std::uint8_t, 16 * 7> packet{};
    packet[0] = static_cast<std::uint8_t>(0x13U << 3); // CHR_TRN
    ppu.apply_sgb_command(packet, packet.size());
    advance_sgb_frames(ppu, 5);

    packet.fill(0);
    packet[0] = static_cast<std::uint8_t>(0x14U << 3); // PCT_TRN
    ppu.apply_sgb_command(packet, packet.size());
    advance_sgb_frames(ppu, 5);

    const auto& border = ppu.sgb_framebuffer();
    check(border[0] == 0xFFFF0000,
          "SGB compositor decodes tile data and RGB555 border palettes");
    check(border[8] == 0xFFFFFFFF,
          "SGB border colour zero uses the screen colour-zero outside the viewport");
    check(border[40 * gameboy::Ppu::sgb_border_width + 48] == 0xFFFFFFFF,
          "SGB compositor overlays transparent border pixels with the GB viewport");

    // A missing transfer still exposes a deterministic centered viewport and
    // the SGB BIOS border instead of collapsing to a black letterbox.
    gameboy::Ppu no_border;
    no_border.set_sgb_mode(true);
    check(no_border.sgb_framebuffer()[8] == 0xFF182A34,
          "SGB compositor exposes the built-in border before cartridge upload");
    check(no_border.sgb_framebuffer()[40 * gameboy::Ppu::sgb_border_width + 48] ==
              0xFFFFFFFF,
          "SGB compositor centers the native viewport before border transfer");
}

void test_default_palette_uses_display_setting() {
    gameboy::Emulator emulator{gameboy::Cartridge{test_rom()}};
    gameboy::DmgPalette layer_colors{
        {0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000},
        {0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00},
        {0xFF0000FF, 0xFF0000FF, 0xFF0000FF, 0xFF0000FF}};
    emulator.bus().set_dmg_palette(layer_colors);
    emulator.bus().write8(0xFF47, 0xE4);
    emulator.bus().write8(0x8000, 0x80);
    emulator.bus().write8(0x8001, 0x80); // Tile 0, first pixel color 3.
    emulator.bus().write8(0xFF40, 0x91);
    emulator.bus().tick(254);
    check(emulator.framebuffer()[0] == 0xFFFF0000,
          "SGB default palette honors the configured display colors");
}

} // namespace

int main() {
    test_transfer_commands_and_guards();
    test_mask_command_is_bounded();
    test_multiplayer_request_and_polling();
    test_multiplayer_command_reaches_joypad();
    test_sgb_first_packet_accepts_start_pulse();
    test_malformed_command_matrix_is_bounded();
    test_palette_command_reaches_video();
    test_palette_and_attribute_transfer_commands();
    test_sgb_border_compositor();
    test_default_palette_uses_display_setting();
    return failures == 0 ? 0 : 1;
}
