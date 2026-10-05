// SPDX-License-Identifier: GPL-3.0-or-later
// Logo-free handoff matrix matching agb_boot_handoff_reference.c's CSV schema.
#include "gameboy/emulator.hpp"
#include <cstdio>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string_view profile = argv[1], startup = argv[2];
    if ((profile != "agb" && profile != "agb0") ||
        (startup != "instant" && startup != "fast" && startup != "animated")) return 2;
    const auto model = profile == "agb" ? gameboy::HardwareModel::agb : gameboy::HardwareModel::agb0;
    const auto mode = startup == "instant" ? gameboy::BootRomMode::post_boot :
        startup == "fast" ? gameboy::BootRomMode::replacement_agb : gameboy::BootRomMode::animated_agb;
    puts("color,license,title,af,bc,de,hl,sp,pc");
    for (unsigned color = 0; color < 2; ++color)
    for (unsigned license = 0; license < 3; ++license)
    for (unsigned title : {0U, 0x0fU, 0x12U, 0x43U, 0x58U, 0xffU}) {
        std::vector<std::uint8_t> rom(32768);
        rom[0x100] = 0x76;
        rom[0x134] = static_cast<std::uint8_t>(title);
        rom[0x143] = color ? 0x80 : 0;
        rom[0x14b] = license == 2 ? 0x33 : static_cast<std::uint8_t>(license);
        if (license == 2) { rom[0x144] = '0'; rom[0x145] = '1'; }
        for (unsigned address = 0x134; address <= 0x14c; ++address) rom[0x14d] -= rom[address] + 1;
        gameboy::Emulator emulator(gameboy::Cartridge(std::move(rom)), model, mode);
        while (emulator.bus().boot_rom_enabled() && emulator.cpu().total_cycles() < 20000000) {
            (void)emulator.step();
            if (emulator.frame_ready()) { (void)emulator.take_audio_samples(); emulator.consume_frame(); }
        }
        if (emulator.bus().boot_rom_enabled()) return 3;
        const auto& r = emulator.cpu().registers();
        printf("%u,%u,%02x,%02x%02x,%02x%02x,%02x%02x,%02x%02x,%04x,%04x\n",
               color, license, title, r.a, r.f, r.b, r.c, r.d, r.e, r.h, r.l, r.sp, r.pc);
    }
}
