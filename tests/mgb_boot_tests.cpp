// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/emulator.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::vector<std::uint8_t> rom(bool zero = false, bool dual = false) {
    std::vector<std::uint8_t> bytes(32768, 0);
    bytes[0x100] = 0xc3; bytes[0x101] = 0x50; bytes[0x102] = 1;
    // Record the actual boot A in WRAM, then HALT. No logo in the header.
    bytes[0x150] = 0xea; bytes[0x151] = 0; bytes[0x152] = 0xc0; bytes[0x153] = 0x76;
    bytes[0x143] = dual ? 0x80 : 0;
    auto checksum = [&] {
        std::uint8_t sum = 0;
        for (unsigned i = 0x134; i <= 0x14c; ++i)
            sum = static_cast<std::uint8_t>(sum - bytes[i] - 1);
        return sum;
    };
    if (zero) bytes[0x134] = checksum();
    bytes[0x14d] = checksum();
    return bytes;
}
void finish(gameboy::Emulator& emulator) {
    for (unsigned i = 0; i < 5000000 && emulator.bus().boot_rom_enabled(); ++i)
        static_cast<void>(emulator.step());
    check(!emulator.bus().boot_rom_enabled(), "Pocket boot never unmapped");
}
}
int main() {
    try {
        using namespace gameboy;
        const auto& dmg = dmg_boot_rom();
        const auto& mgb = mgb_boot_rom();
        unsigned differences = 0;
        for (std::size_t i = 0; i < dmg.size(); ++i) if (dmg[i] != mgb[i]) {
            ++differences;
            check(i > 0 && dmg[i - 1] == 0x3e && dmg[i] == 1 && mgb[i] == 0xff,
                  "Pocket image changed more than the LD A immediate");
        }
        check(differences == 1, "Pocket image must differ by exactly one byte");
        differences = 0;
        const auto& animated_dmg = dmg_animated_boot_rom();
        const auto& animated_mgb = mgb_animated_boot_rom();
        for (std::size_t i = 0; i < animated_dmg.size(); ++i) if (animated_dmg[i] != animated_mgb[i]) {
            ++differences;
            check(i > 0 && animated_dmg[i - 1] == 0x3e && animated_dmg[i] == 1 && animated_mgb[i] == 0xff,
                  "animated Pocket image changed more than the LD A immediate");
        }
        check(differences == 1, "animated Pocket image must differ by exactly one byte");
        check(mgb[0xfe] == 0xe0 && mgb[0xff] == 0x50, "Pocket unmap moved");
        Emulator instant(Cartridge(rom()), HardwareModel::mgb);
        check(!instant.bus().boot_rom_enabled() && instant.cpu().registers().a == 0xff,
              "default Pocket startup no longer uses its instant profile");
        for (bool zero : {false, true}) for (auto mode : {BootRomMode::replacement, BootRomMode::animated}) {
            Emulator pocket(Cartridge(rom(zero)), HardwareModel::mgb, mode);
            Emulator baseline(Cartridge(rom(zero)), HardwareModel::dmg, mode);
            check(pocket.cpu().registers().pc == 0 && pocket.bus().boot_rom_enabled(), "Pocket bypassed CPU boot");
            for (unsigned i = 0; i < 137; ++i) static_cast<void>(pocket.step());
            const auto saved = pocket.save_state();
            Emulator restored(Cartridge(rom(zero)), HardwareModel::mgb);
            restored.load_state(saved);
            check(restored.save_state() == saved, "Pocket mid-boot state does not round trip");
            finish(pocket); finish(restored); finish(baseline);
            check(restored.save_state() == pocket.save_state(), "Pocket restored continuation differs");
            const auto& r = pocket.cpu().registers();
            check(r.pc == 0x100 && r.sp == 0xfffe && r.a == 0xff &&
                  r.f == (zero ? 0x80 : 0xb0) && r.b == 0 && r.c == 0x13 &&
                  r.d == 0 && r.e == 0xd8 && r.h == 1 && r.l == 0x4d,
                  "Pocket CPU handoff differs from the documented contract");
            check(pocket.cpu().total_cycles() == baseline.cpu().total_cycles(), "Pocket changes boot timing");
            check(pocket.bus().debug_divider_counter() == 0xabc8 &&
                  pocket.bus().debug_ppu_dot() == 396 && pocket.bus().read8(0xff41) == 0x85,
                  "Pocket timer/LCD handoff differs");
            check(pocket.bus().debug_apu_clock_state() == baseline.bus().debug_apu_clock_state(), "Pocket changes APU handoff");
            for (unsigned address = 0xff00; address <= 0xffff; ++address)
                check(pocket.bus().read8(static_cast<std::uint16_t>(address)) == baseline.bus().read8(static_cast<std::uint16_t>(address)),
                      "Pocket changes peripheral/HRAM handoff");
            for (unsigned i = 0; i < 8192; ++i)
                check(pocket.bus().debug_read_vram(0, static_cast<std::uint16_t>(i)) == 0, "Pocket leaves VRAM uncleared");
            check(pocket.framebuffer() == baseline.framebuffer(), "Pocket framebuffer differs");
            check(pocket.take_audio_samples() == baseline.take_audio_samples(), "Pocket changes startup PCM");
            for (unsigned i = 0; i < 3; ++i) static_cast<void>(pocket.step());
            check(pocket.bus().read8(0xc000) == 0xff, "cartridge cannot detect Pocket A");
            pocket.bus().write8(0xff50, 0);
            check(!pocket.bus().boot_rom_enabled(), "Pocket FF50 remapped");
            pocket.reset();
            check(pocket.bus().boot_rom_enabled() && pocket.cpu().registers().pc == 0,
                  "Pocket reset did not restart firmware");
            finish(pocket);
            check(pocket.cpu().registers().a == 0xff, "Pocket reset used DMG image");
        }
        auto invalid = rom(); invalid[0x14d] ^= 1;
        Emulator rejected(Cartridge(invalid), HardwareModel::mgb, BootRomMode::replacement);
        for (unsigned i = 0; i < 50000; ++i) static_cast<void>(rejected.step());
        check(rejected.bus().boot_rom_enabled() && rejected.bus().read8(0xff40) == 0, "Pocket accepts invalid checksum");
        for (auto startup : {gbb::StartupMode::replacement, gbb::StartupMode::animated}) {
            gbb::CoreLoadOptions options; options.hardware_model = "mgb"; options.startup_mode = startup;
            auto core = gbb::built_in_core_registry().create(rom(false, true), options);
            auto* pocket = gbb::gameboy_emulator(core.get());
            check(pocket && pocket->hardware_model() == HardwareModel::mgb && pocket->bus().boot_rom_enabled(), "factory ignores Pocket startup");
            check(pocket->startup_animation_active() == (startup == gbb::StartupMode::animated), "Pocket animation selection differs");
            finish(*pocket);
            check(pocket->cpu().registers().a == 0xff, "factory used wrong firmware");
        }
        std::cout << "Pocket image, CPU handoff, reset, state and factory contracts passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
