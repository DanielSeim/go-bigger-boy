// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/emulator.hpp"
#include "gameboy/boot_splash.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"
#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<std::uint8_t> rom(bool color, unsigned license = 0, unsigned title = 0) {
    std::vector<std::uint8_t> bytes(32768, 0);
    bytes[0x100] = 0x76;
    bytes[0x134] = static_cast<std::uint8_t>(title);
    bytes[0x143] = color ? 0x80 : 0;
    bytes[0x14b] = static_cast<std::uint8_t>(license);
    if (license == 0x33) { bytes[0x144] = '0'; bytes[0x145] = '1'; }
    unsigned checksum = 0;
    for (unsigned i = 0x134; i <= 0x14c; ++i) checksum = (checksum - bytes[i] - 1) & 255;
    bytes[0x14d] = static_cast<std::uint8_t>(checksum);
    return bytes;
}
void finish(gameboy::Emulator& emulator, bool* heard = nullptr) {
    for (unsigned i = 0; i < 3000000 && emulator.bus().boot_rom_enabled(); ++i) {
        (void)emulator.step();
        if (emulator.frame_ready()) {
            (void)emulator.framebuffer();
            const auto samples = emulator.take_audio_samples();
            if (heard) *heard |= std::any_of(samples.begin(), samples.end(), [](auto v) { return v != 0; });
            emulator.consume_frame();
        }
    }
    check(!emulator.bus().boot_rom_enabled(), "CGB firmware did not unmap");
}
}
int main() {
    try {
        using namespace gameboy;
        for (auto model : {HardwareModel::cgb0, HardwareModel::cgb_c, HardwareModel::cgb_e, HardwareModel::cgb,
                           HardwareModel::agb0, HardwareModel::agb}) {
            for (bool color : {false, true}) for (auto mode : {BootRomMode::replacement_cgb, BootRomMode::animated_cgb}) {
                auto bytes = rom(color);
                Emulator emulator(Cartridge(bytes), model, mode);
                check(emulator.cpu().registers().pc == 0 && emulator.bus().boot_rom_enabled(), "CGB did not start cold");
                for (unsigned i = 0; i < 137; ++i) (void)emulator.step();
                auto saved = emulator.save_state();
                Emulator restored(Cartridge(bytes), model);
                restored.load_state(saved);
                check(restored.save_state() == saved, "cold CGB state differs after restore");
                bool heard = false;
                finish(emulator, &heard); finish(restored);
                check(emulator.save_state() == restored.save_state(), "CGB continuation differs after restore");
                const auto& r = emulator.cpu().registers();
                check(r.a == 0x11 && r.f == (is_agb_hardware(model) ? 0 : 0x80) &&
                    r.b == (is_agb_hardware(model) ? 1 : 0) && r.c == 0 &&
                    r.d == (color ? 0xff : 0) && r.e == (color ? 0x56 : 8) &&
                    r.h == 0 && r.l == (color ? 0x0d : 0x7c) && r.sp == 0xfffe && r.pc == 0x100,
                    "CGB cartridge register handoff differs");
                check(emulator.bus().cgb_mode() == color && emulator.bus().read8(0xff40) == 0x91,
                    "CGB compatibility/LCD handoff differs");
                check(!emulator.startup_animation_active(), "CGB splash outlived firmware");
                if (mode == BootRomMode::animated_cgb) {
                    check(heard, "CGB intro has no chime");
                    check(emulator.cpu().total_cycles() > 168 * boot_splash_frame_cycles &&
                          emulator.cpu().total_cycles() < 200 * boot_splash_frame_cycles, "CGB intro duration outside design bounds");
                }
                if (color) {
                    check(emulator.bus().read8(0xff4f) == 0xfe, "CGB did not restore bank zero");
                    for (unsigned i = 0; i < 16; ++i)
                        check(emulator.bus().read8(static_cast<std::uint16_t>(0xff30 + i)) ==
                            (model == HardwareModel::cgb0 ? 0 : i % 2 ? 255 : 0), "CGB wave initialization differs");
                }
                const auto clocks = emulator.cpu().total_cycles();
                emulator.reset(); finish(emulator);
                check(emulator.cpu().total_cycles() == clocks, "CGB reset changed firmware timing");
            }
        }
        for (auto model : {HardwareModel::agb0, HardwareModel::agb}) {
            for (unsigned license : {0U, 1U, 0x33U})
            for (unsigned title : {0U, 0x0fU, 0x12U, 0x43U, 0x58U, 0xffU}) {
                const auto checksum = license ? title : 0;
                const auto expected_b = static_cast<std::uint8_t>(checksum + 1);
                const auto expected_f = (expected_b == 0 ? 0x80 : 0) | ((checksum & 15) == 15 ? 0x20 : 0);
                for (auto mode : {BootRomMode::post_boot, BootRomMode::replacement_agb}) {
                    Emulator emulator(Cartridge(rom(false, license, title)), model, mode);
                    finish(emulator);
                    const auto& r = emulator.cpu().registers();
                    const bool legacy = checksum == 0x43 || checksum == 0x58;
                    check(r.a == 0x11 && r.b == expected_b && r.f == expected_f && r.c == 0 &&
                          r.d == 0 && r.e == 8 && r.h == (legacy ? 0x99 : 0) &&
                          r.l == (legacy ? 0x1a : 0x7c) && r.sp == 0xfffe && r.pc == 0x100,
                          "AGB compatibility INC/title/legacy handoff differs");
                }
            }
            for (auto mode : {BootRomMode::post_boot, BootRomMode::replacement_agb}) {
                Emulator emulator(Cartridge(rom(true)), model, mode);
                finish(emulator);
                const auto& r = emulator.cpu().registers();
                check(r.a == 0x11 && r.f == 0 && r.b == 1 && r.c == 0 && r.d == 0xff &&
                      r.e == 0x56 && r.h == 0 && r.l == 0x0d, "AGB native handoff differs");
            }
            auto unlicensed = rom(false, 0x33, 0xff);
            unlicensed[0x145] = '2';
            unsigned sum = 0;
            for (unsigned address = 0x134; address <= 0x14c; ++address)
                sum = (sum - unlicensed[address] - 1) & 255;
            unlicensed[0x14d] = static_cast<std::uint8_t>(sum);
            Emulator rejected_license(Cartridge(unlicensed), model, BootRomMode::replacement_agb);
            finish(rejected_license);
            check(rejected_license.cpu().registers().b == 1 && rejected_license.cpu().registers().f == 0,
                  "AGB must not checksum non-Nintendo new-license titles");
            auto invalid_rom = rom(true); invalid_rom[0x14d] ^= 1;
            Emulator invalid_agb(Cartridge(invalid_rom), model, BootRomMode::replacement_agb);
            for (unsigned i = 0; i < 100000; ++i) (void)invalid_agb.step();
            check(invalid_agb.bus().boot_rom_enabled() && invalid_agb.bus().read8(0xff40) == 0,
                  "AGB invalid checksum entered cartridge");
            Emulator muted(Cartridge(rom(true)), model, BootRomMode::animated_agb);
            muted.set_audio_enabled(false);
            finish(muted);
            check(muted.take_audio_samples().empty(), "muted AGB intro generated audio");
            muted.reset(); muted.set_button(Button::start, true);
            check(!muted.startup_animation_active() && muted.bus().boot_rom_enabled(),
                  "AGB skip changed firmware mapping");
        }
        for (auto license : {1U, 0x33U}) for (auto title : {0x43U, 0x58U, 0x12U}) {
            Emulator emulator(Cartridge(rom(false, license, title)), HardwareModel::cgb_e, BootRomMode::replacement_cgb);
            finish(emulator);
            check(emulator.cpu().registers().b == title &&
                emulator.cpu().registers().h == (title == 0x12 ? 0 : 0x99) &&
                emulator.cpu().registers().l == (title == 0x12 ? 0x7c : 0x1a), "licensed compatibility handoff differs");
        }
        for (auto profile : {"cgb-e", "agb0", "agb"})
        for (bool color : {false, true}) for (auto mode : {gbb::StartupMode::replacement_dmg, gbb::StartupMode::animated_dmg}) {
            gbb::CoreLoadOptions options; options.hardware_model = profile; options.startup_mode = mode;
            auto core = gbb::built_in_core_registry().create(rom(color), options);
            check(gbb::gameboy_emulator(core.get())->bus().boot_rom_enabled(), "factory ignored CGB boot");
            if (mode == gbb::StartupMode::animated_dmg)
                check(core->video_frame_native_colors(), "color intro must bypass monochrome palette mapping");
        }
        auto broken = rom(true); broken[0x14d] ^= 1;
        Emulator invalid(Cartridge(broken), HardwareModel::cgb_e, BootRomMode::animated_cgb);
        for (unsigned i = 0; i < 100000; ++i) (void)invalid.step();
        check(invalid.bus().boot_rom_enabled() && invalid.bus().read8(0xff40) == 0, "invalid CGB checksum entered cartridge");

        auto frames = std::make_unique<std::array<Ppu::Framebuffer, 5>>();
        auto& blank = (*frames)[0];
        auto& partial = (*frames)[1];
        auto& full = (*frames)[2];
        auto& gone = (*frames)[3];
        render_boot_splash(blank, 0, HardwareModel::cgb);
        render_boot_splash(partial, 90, HardwareModel::cgb);
        render_boot_splash(full, 120, HardwareModel::cgb);
        render_boot_splash(gone, 177, HardwareModel::cgb);
        check(std::all_of(blank.begin(), blank.end(), [](auto p) { return p == 0xffffffff; }) && blank == gone,
              "CGB intro must fade from and to white");
        check(partial != blank && partial != full && full != blank, "CGB rainbow reveal is missing");
        auto& held = (*frames)[4];
        render_boot_splash(held, 130, HardwareModel::cgb);
        check(held == full, "CGB logo must hold still while the chime decays");
        check(std::count(full.begin(), full.end(), 0xff0000ffU) > 1200,
              "CGB bold lettering must settle to saturated blue");
        for (auto color : {0xffff0000U, 0xffff00ffU, 0xff00ff00U, 0xffffff00U})
            check(std::find(partial.begin(), partial.end(), color) != partial.end(),
                  "CGB reveal must cycle through the full rainbow");
        check(full[48 * 160 + 18] == 0xff0000ffU && full[68 * 160 + 14] == 0xff0000ffU,
              "CGB logo must have the intended oblique silhouette and vertical placement");
        render_boot_splash(held, 160, HardwareModel::cgb);
        check(held != full && held != blank && held[48 * 160 + 18] == 0xff7474ffU,
              "CGB palette fade must lighten the blue logo without moving it");
        render_boot_splash(held, 55, HardwareModel::cgb);
        check(held[104 * 160 + 73] == 0xff000000U && held[48 * 160 + 18] == 0xffffffffU,
              "CGB original GBB footer must precede the main logo");
        for (auto note : {cgb_splash_first_note_cycle, cgb_splash_second_note_cycle}) {
            const auto start = boot_splash_sample_time(note);
            unsigned crossings = 0;
            for (auto i = start + 100; i < start + 1060; ++i)
                crossings += boot_splash_sample(i, HardwareModel::cgb) > 0 &&
                             boot_splash_sample(i - 1, HardwareModel::cgb) <= 0;
            check(note == cgb_splash_first_note_cycle ? crossings >= 19 && crossings <= 23
                  : crossings >= 39 && crossings <= 44, "CGB two-note chime pitch differs");
        }
        Emulator audible(Cartridge(rom(true)), HardwareModel::cgb_e, BootRomMode::animated_cgb);
        while (audible.cpu().total_cycles() < cgb_splash_first_note_cycle + 10000) {
            (void)audible.step();
            if (audible.frame_ready()) { (void)audible.take_audio_samples(); audible.consume_frame(); }
        }
        Emulator receiver(Cartridge(rom(true)), HardwareModel::cgb_e);
        receiver.load_state(audible.save_state());
        check(receiver.framebuffer() == audible.framebuffer(), "CGB restored fade differs");
        (void)audible.take_audio_samples();
        for (unsigned i = 0; i < 1000; ++i) check(receiver.step() == audible.step(), "CGB restored clock differs");
        check(receiver.take_audio_samples() == audible.take_audio_samples(), "CGB restored chime differs");
        audible.set_audio_enabled(false); audible.reset(); finish(audible);
        check(audible.take_audio_samples().empty(), "muted CGB intro generated audio");
        Emulator skipped(Cartridge(rom(true)), HardwareModel::cgb_e, BootRomMode::animated_cgb);
        skipped.set_button(Button::start, true);
        check(!skipped.startup_animation_active() && skipped.bus().boot_rom_enabled(), "skip altered firmware startup");
        std::cout << "CGB/AGB firmware, compatibility, reveal, chime, restore and reset passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
