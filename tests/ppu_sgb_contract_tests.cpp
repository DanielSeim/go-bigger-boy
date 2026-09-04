#include "gameboy/emulator.hpp"
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

void test_transfer_commands_and_guards() {
    gameboy::Ppu ppu;
    std::array<std::uint8_t, 16 * 7> packet{};
    packet[0] = static_cast<std::uint8_t>(0x13U << 3); // CHR_TRN

    ppu.debug_write_vram(0, 0x0000, 0xA5);
    ppu.debug_write_vram(0, 0x0FFF, 0x5A);
    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0,
          "SGB commands are ignored while SGB mode is disabled");

    ppu.set_sgb_mode(true);
    ppu.apply_sgb_command(packet, 1);
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0,
          "truncated SGB packets do not modify transfer latches");

    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_border_tile(0x0000) == 0xA5 &&
              ppu.debug_read_sgb_border_tile(0x0FFF) == 0x5A,
          "CHR_TRN copies the first tile-data bank");

    packet[1] = 1;
    ppu.debug_write_vram(0, 0x0000, 0x3C);
    ppu.debug_write_vram(0, 0x0FFF, 0xC3);
    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_border_tile(0x1000) == 0x3C &&
              ppu.debug_read_sgb_border_tile(0x1FFF) == 0xC3,
          "CHR_TRN selects the second tile-data bank");

    packet[0] = static_cast<std::uint8_t>(0x14U << 3); // PCT_TRN
    ppu.debug_write_vram(0, 0x0000, 0x11);
    ppu.debug_write_vram(0, 0x0FFF, 0xEE);
    ppu.apply_sgb_command(packet, packet.size());
    check(ppu.debug_read_sgb_border_pct(0x0000) == 0x11 &&
              ppu.debug_read_sgb_border_pct(0x0FFF) == 0xEE,
          "PCT_TRN copies the complete border payload");
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
    test_malformed_command_matrix_is_bounded();
    test_palette_command_reaches_video();
    test_default_palette_uses_display_setting();
    return failures == 0 ? 0 : 1;
}
