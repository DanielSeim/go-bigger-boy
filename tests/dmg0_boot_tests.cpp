// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/emulator.hpp"
#include "gameboy/boot_splash.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::uint8_t> rom(bool zero = false) {
    std::vector<std::uint8_t> bytes(32768, 0);
    bytes[0x100] = 0x76;
    auto checksum = [&] { unsigned sum = 0; for (unsigned i = 0x134; i <= 0x14c; ++i) sum = (sum - bytes[i] - 1) & 255; return sum; };
    if (zero) bytes[0x134] = static_cast<std::uint8_t>(checksum());
    bytes[0x14d] = static_cast<std::uint8_t>(checksum());
    return bytes;
}
void finish(gameboy::Emulator& emulator, bool* heard = nullptr) {
    for (unsigned i = 0; i < 5000000 && emulator.bus().boot_rom_enabled(); ++i) {
        (void)emulator.step();
        if (emulator.frame_ready()) {
            (void)emulator.framebuffer();
            const auto samples = emulator.take_audio_samples();
            if (heard) for (auto sample : samples) *heard |= sample != 0;
            emulator.consume_frame();
        }
    }
    check(!emulator.bus().boot_rom_enabled(), "DMG0 never unmapped");
}
}
int main() {
    try {
        using namespace gameboy;
        for (bool zero : {false, true}) for (auto mode : {BootRomMode::replacement_dmg0, BootRomMode::animated_dmg0}) {
            Emulator emulator(Cartridge(rom(zero)), HardwareModel::dmg0, mode);
            check(emulator.bus().boot_rom_enabled() && emulator.cpu().registers().pc == 0, "DMG0 did not cold boot");
            for (unsigned i = 0; i < 137; ++i) (void)emulator.step();
            const auto saved = emulator.save_state();
            Emulator restored(Cartridge(rom(zero)), HardwareModel::dmg0);
            restored.load_state(saved);
            check(restored.save_state() == saved, "DMG0 state roundtrip differs");
            bool heard = false;
            finish(emulator, &heard); finish(restored);
            if (mode == BootRomMode::animated_dmg0) check(heard, "DMG0 animated boot has no chime");
            check(restored.save_state() == emulator.save_state(), "DMG0 continuation differs");
            const auto& r = emulator.cpu().registers();
            check(r.a == 1 && r.f == 0 && r.b == 255 && r.c == 0x13 && r.d == 0 && r.e == 0xc1 && r.h == 0x84 && r.l == 3 && r.pc == 0x100 && r.sp == 0xfffe, "DMG0 CPU handoff differs");
            std::cout << "handoff clocks=" << emulator.cpu().total_cycles() << " divider=" << emulator.bus().debug_divider_counter() << " LY=" << unsigned(emulator.bus().read8(0xff44)) << " dot=" << emulator.bus().debug_ppu_dot() << '\n';
            check(emulator.bus().debug_divider_counter() == 0x1828 && emulator.bus().read8(0xff44) == 0x91 && emulator.bus().read8(0xff41) == 0x81 && emulator.bus().debug_ppu_dot() == 92, "DMG0 divider/LCD handoff differs");
            check(emulator.cpu().total_cycles() == (mode == BootRomMode::animated_dmg0 ? 24863304U : 4285000U), "DMG0 boot duration changed");
            emulator.reset(); finish(emulator);
            check(emulator.cpu().registers().b == 255 && emulator.cpu().registers().f == 0, "DMG0 reset used later firmware");
        }
        for (auto mode : {BootRomMode::replacement_dmg0, BootRomMode::animated_dmg0}) {
            auto bytes = rom(); bytes[0x14d] ^= 1;
            Emulator invalid(Cartridge(bytes), HardwareModel::dmg0, mode);
            bool white = false, black = false;
            for (unsigned i = 0; i < 1000000; ++i) {
                (void)invalid.step();
                if (invalid.frame_ready()) {
                    const auto& frame = invalid.framebuffer();
                    white |= frame[0] == 0xffffffff;
                    black |= frame[0] == 0xff000000;
                    invalid.consume_frame();
                }
            }
            check(invalid.bus().boot_rom_enabled() && white && black && !invalid.startup_animation_active(), "DMG0 failed checksum must blink, not launch cartridge or GBB intro");
        }
        for (auto mode : {gbb::StartupMode::replacement_dmg, gbb::StartupMode::animated_dmg}) {
            gbb::CoreLoadOptions options; options.hardware_model = "dmg0"; options.startup_mode = mode;
            auto core = gbb::built_in_core_registry().create(rom(), options);
            auto* emulator = gbb::gameboy_emulator(core.get());
            check(emulator && emulator->bus().boot_rom_enabled(), "factory ignores DMG0 boot");
            finish(*emulator);
            check(emulator->cpu().registers().b == 255, "factory selects wrong boot image");
        }
        Emulator audible(Cartridge(rom()), HardwareModel::dmg0, BootRomMode::animated_dmg0);
        while (audible.cpu().total_cycles() < 18'600'000) {
            (void)audible.step();
            if (audible.frame_ready()) { (void)audible.take_audio_samples(); audible.consume_frame(); }
        }
        const auto mid_chime = audible.save_state();
        Emulator receiver(Cartridge(rom()), HardwareModel::dmg0);
        receiver.load_state(mid_chime);
        (void)audible.take_audio_samples();
        check(receiver.framebuffer() == audible.framebuffer(), "restored DMG0 glyph position differs");
        for (unsigned i = 0; i < 1000; ++i) check(receiver.step() == audible.step(), "DMG0 restore timing differs");
        check(receiver.take_audio_samples() == audible.take_audio_samples(), "restored DMG0 chime differs");
        audible.set_audio_enabled(false); audible.reset();
        finish(audible);
        check(audible.take_audio_samples().empty(), "muted DMG0 boot generated audio");
        for (auto note : {18'523'904U, 18'875'268U}) {
            auto sample = boot_splash_sample_time(note);
            check(boot_splash_sample(sample + 500, HardwareModel::dmg0) != 0, "missing DMG0 note");
            unsigned crossings = 0;
            for (auto i = sample + 100; i < sample + 1060; ++i)
                crossings += boot_splash_sample(i, HardwareModel::dmg0) > 0 && boot_splash_sample(i - 1, HardwareModel::dmg0) <= 0;
            check(note == 18'523'904U ? crossings >= 19 && crossings <= 23 : crossings >= 39 && crossings <= 44, "DMG0 chime pitch differs");
        }
        std::cout << "DMG0 firmware, handoff, failure blink, restore, reset and factory passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
