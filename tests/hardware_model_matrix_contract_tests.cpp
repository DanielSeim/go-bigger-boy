#include "gameboy/emulator.hpp"
#include "gameboy/hardware_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom(const bool cgb, const bool sgb) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr char title[] = "MODEL MATRIX";
    for (std::size_t index = 0; title[index] != '\0'; ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    rom[0x143] = cgb ? 0x80 : 0x00;
    rom[0x146] = sgb ? 0x03 : 0x00;
    return rom;
}

struct ExpectedRegisters {
    std::uint8_t a;
    std::uint8_t f;
    std::uint8_t b;
    std::uint8_t c;
    std::uint8_t d;
    std::uint8_t e;
    std::uint8_t h;
    std::uint8_t l;
};

struct ModelCase {
    gameboy::HardwareModel model;
    const char* name;
    ExpectedRegisters registers;
    std::uint8_t divider;
    std::uint32_t serial_phase;
    bool cgb_hardware;
    bool sgb_mode;
    bool pulse_one_enabled;
};

constexpr std::array<ModelCase, 9> model_cases{{
    {gameboy::HardwareModel::dmg0, "DMG0",
     {0x01, 0x00, 0xFF, 0x13, 0x00, 0xC1, 0x84, 0x03}, 0x18, 0, false,
     false, true},
    {gameboy::HardwareModel::dmg, "DMG",
     {0x01, 0xB0, 0x00, 0x13, 0x00, 0xD8, 0x01, 0x4D}, 0xAB, 460, false,
     false, true},
    {gameboy::HardwareModel::mgb, "MGB",
     {0xFF, 0xB0, 0x00, 0x13, 0x00, 0xD8, 0x01, 0x4D}, 0xAB, 460, false,
     false, true},
    {gameboy::HardwareModel::sgb, "SGB",
     {0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0xC0, 0x60}, 0xD8, 0, false,
     true, false},
    {gameboy::HardwareModel::sgb2, "SGB2",
     {0xFF, 0x00, 0x00, 0x14, 0x00, 0x00, 0xC0, 0x60}, 0xD8, 0, false,
     true, false},
    {gameboy::HardwareModel::cgb0, "CGB0",
     {0x11, 0x80, 0x00, 0x00, 0xFF, 0x56, 0x00, 0x0D}, 0x20, 0, true,
     false, true},
    {gameboy::HardwareModel::cgb_c, "CGB-C",
     {0x11, 0x80, 0x00, 0x00, 0xFF, 0x56, 0x00, 0x0D}, 0x1E, 0, true,
     false, true},
    {gameboy::HardwareModel::cgb_e, "CGB-E",
     {0x11, 0x80, 0x00, 0x00, 0xFF, 0x56, 0x00, 0x0D}, 0x1E, 0, true,
     false, true},
    {gameboy::HardwareModel::cgb, "CGB alias",
     {0x11, 0x80, 0x00, 0x00, 0xFF, 0x56, 0x00, 0x0D}, 0x1E, 0, true,
     false, true},
}};

void check_registers(const gameboy::CpuRegisters& actual,
                     const ExpectedRegisters expected,
                     const std::string& model) {
    check(actual.a == expected.a && actual.f == expected.f &&
              actual.b == expected.b && actual.c == expected.c &&
              actual.d == expected.d && actual.e == expected.e &&
              actual.h == expected.h && actual.l == expected.l,
          model + " boot CPU registers match its hardware profile");
    check(actual.sp == 0xFFFE && actual.pc == 0x0100,
          model + " boot stack and program counters match");
}

void test_cartridge_register_handoff() {
    // Authored cartridge witness: store the incoming DE/HL without modifying
    // either pair. Expected values were checked against the external boot path.
    constexpr std::array<std::uint8_t, 19> witness{
        0x7a, 0xea, 0x00, 0xc0, 0x7b, 0xea, 0x01, 0xc0,
        0x7c, 0xea, 0x02, 0xc0, 0x7d, 0xea, 0x03, 0xc0,
        0xc3, 0x10, 0x01};
    for (const auto model : {gameboy::HardwareModel::automatic,
                            gameboy::HardwareModel::cgb0,
                            gameboy::HardwareModel::cgb_c,
                            gameboy::HardwareModel::cgb_e,
                            gameboy::HardwareModel::cgb}) {
        for (const unsigned flag : {0U, 0x80U, 0xc0U}) {
            if (model == gameboy::HardwareModel::automatic && flag == 0) continue;
            auto rom = test_rom(flag != 0, false);
            rom[0x143] = static_cast<std::uint8_t>(flag);
            std::copy(witness.begin(), witness.end(), rom.begin() + 0x100);
            gameboy::Emulator emulator{gameboy::Cartridge{std::move(rom)}, model};
            const std::array<std::uint8_t, 4> expected = flag
                ? std::array<std::uint8_t, 4>{0xff, 0x56, 0, 0x0d}
                : std::array<std::uint8_t, 4>{0, 8, 0, 0x7c};
            const auto initial = emulator.save_state();
            for (unsigned run = 0; run < 3; ++run) {
                for (unsigned step = 0; step < 8; ++step) (void)emulator.step();
                for (unsigned byte = 0; byte < expected.size(); ++byte)
                    check(emulator.bus().read8(static_cast<std::uint16_t>(0xc000 + byte)) == expected[byte],
                          "cartridge observes native/compatibility DE/HL at startup, reset and restore");
                if (run == 0) emulator.reset();
                if (run == 1) emulator.load_state(initial);
            }
        }
    }
}

void test_explicit_profiles() {
    for (const auto& model_case : model_cases) {
        const auto cgb_cartridge = model_case.cgb_hardware;
        const auto sgb_cartridge = model_case.sgb_mode;
        gameboy::Emulator emulator{
            gameboy::Cartridge{test_rom(cgb_cartridge, sgb_cartridge)},
            model_case.model};
        const auto prefix = std::string{model_case.name} + ": ";
        check_registers(emulator.cpu().registers(), model_case.registers,
                        prefix);
        check(emulator.bus().read8(0xFF04) == model_case.divider,
              prefix + "post-boot DIV high byte matches the profile");
        check(emulator.bus().serial_port().phase() == model_case.serial_phase,
              prefix + "serial divider phase matches the profile");
        check((emulator.bus().read8(0xFF00) ==
               (model_case.sgb_mode || model_case.cgb_hardware ? 0xFF : 0xCF)),
              prefix + "JOYP post-boot selection matches the profile");
        check(((emulator.bus().read8(0xFF26) & 0x01) != 0) ==
                  model_case.pulse_one_enabled,
              prefix + "APU channel-one startup state matches the profile");

        const auto state = emulator.save_state();

        if (model_case.cgb_hardware) {
            check(emulator.bus().cgb_mode(),
                  prefix + "CGB cartridge enters CGB mode");
            check(emulator.bus().read8(0xFF4F) == 0xFE &&
                      emulator.bus().read8(0xFF68) == 0xC8 &&
                      emulator.bus().read8(0xFF6A) == 0xD0,
                  prefix + "CGB-only registers expose the profile defaults");
        } else {
            check(!emulator.bus().cgb_mode() &&
                      emulator.bus().read8(0xFF4F) == 0xFF,
                  prefix + "monochrome cartridge remains outside CGB mode");
        }

        // Exercise the shared frame and serial boundaries for every profile,
        // rather than limiting the matrix to register snapshots.  The
        // profile-specific timing above must not prevent a frame publication
        // or a normal disconnected serial byte from completing.
        emulator.bus().tick(70224);
        check(emulator.bus().frame_ready(),
              prefix + "PPU publishes a frame at the profile's frame boundary");
        emulator.bus().consume_frame();
        emulator.bus().write8(0xFF01, 0xA5);
        emulator.bus().write8(0xFF02, 0x81);
        emulator.bus().tick(4096);
        check(!emulator.bus().serial_port().transfer_active() &&
                  emulator.bus().serial_port().transfers_completed() == 1 &&
                  emulator.bus().read8(0xFF01) == 0xFF,
              prefix + "serial transfer completes at the profile's normal rate");

        emulator.bus().tick(97);
        emulator.load_state(state);
        check_registers(emulator.cpu().registers(), model_case.registers,
                        prefix + "save state: ");
        check(emulator.bus().read8(0xFF04) == model_case.divider &&
                  emulator.bus().serial_port().phase() == model_case.serial_phase,
              prefix + "save state restores model-specific timing state");
    }
}

void test_color_boot_mode_phases() {
    using gameboy::HardwareModel;
    for (const auto model : {HardwareModel::cgb0, HardwareModel::cgb_c,
                             HardwareModel::cgb_e, HardwareModel::agb0, HardwareModel::agb}) {
        for (bool color : {false, true}) {
            gameboy::Emulator emulator{gameboy::Cartridge{test_rom(color, false)}, model};
            const bool early = model == HardwareModel::cgb0;
            const bool agb = gameboy::is_agb_hardware(model);
            const auto counter = color ? (early ? 0x20AC : agb ? 0x1EA4 : 0x1EA0)
                                       : (early ? 0x2880 : agb ? 0x2678 : 0x2674);
            check(emulator.bus().debug_divider_counter() == counter,
                  "color boot divider depends on native versus compatibility mode");
            check(emulator.bus().read8(0xFF44) == (color ? 144 : 148) &&
                      emulator.bus().debug_ppu_dot() == (color ? (agb ? 172U : 168U) : (agb ? 360U : 356U)) &&
                      emulator.bus().debug_ppu_mode() == 1,
                  "color boot handoff is in the mode-specific VBlank phase");
        }
    }
}

void test_power_on_work_ram() {
    for (const auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::cgb_e}) {
        auto image = test_rom(gameboy::is_cgb_hardware(model), false);
        gameboy::Emulator first{gameboy::Cartridge{image}, model};
        gameboy::Emulator second{gameboy::Cartridge{image}, model};
        bool mixed = false;
        const auto initial = first.bus().read8(0xC000);
        for (unsigned address = 0xC000; address < 0xE000; ++address) {
            const auto byte = first.bus().read8(static_cast<std::uint16_t>(address));
            mixed |= byte != 0 && byte != 255;
            check(byte == second.bus().read8(static_cast<std::uint16_t>(address)),
                  "power-on WRAM is reproducible across independent machines");
        }
        check(mixed, "undefined WRAM is not silently zero or FF filled");
        first.bus().write8(0xC000, initial ^ 255);
        first.reset();
        check(first.bus().read8(0xC000) == (initial ^ 255), "soft reset retains cartridge WRAM");
        first.bus().initialize_dmg_power_on();
        check(first.bus().read8(0xC000) == initial, "cold reset recreates deterministic WRAM");
    }
}

void test_monochrome_header_logo() {
    for (const auto model : {gameboy::HardwareModel::dmg, gameboy::HardwareModel::mgb}) {
        auto image = test_rom(false, false);
        image[0x104] = 0xA5;
        gameboy::Emulator emulator{gameboy::Cartridge{image}, model};
        constexpr std::array<std::uint8_t, 8> expected{0xCC, 0, 0xCC, 0, 0x33, 0, 0x33, 0};
        for (unsigned i = 0; i < expected.size(); ++i)
            check(emulator.bus().debug_read_vram(0, 0x10 + i) == expected[i],
                  "monochrome boot tiles derive from cartridge header pixels");
        check(emulator.bus().debug_read_vram(0, 0x1904) == 1 &&
                  emulator.bus().debug_read_vram(0, 0x1910) == 25 &&
                  emulator.bus().debug_read_vram(0, 0x1924) == 13,
              "monochrome boot map retains both logo rows and trademark position");
    }
}

void test_automatic_selection() {
    gameboy::Emulator dmg{gameboy::Cartridge{test_rom(false, false)}};
    check(!dmg.bus().cgb_mode() && dmg.bus().read8(0xFF00) == 0xCF,
          "automatic selection chooses the DMG profile for ordinary software");

    gameboy::Emulator sgb{gameboy::Cartridge{test_rom(false, true)}};
    check(!sgb.bus().cgb_mode() && sgb.bus().read8(0xFF00) == 0xFF,
          "automatic selection chooses the SGB profile for SGB software");

    gameboy::Emulator cgb{gameboy::Cartridge{test_rom(true, true)}};
    check(cgb.bus().cgb_mode() && cgb.bus().read8(0xFF4F) == 0xFE,
          "automatic selection gives CGB-capable software priority over SGB");

    gameboy::Emulator explicit_sgb{gameboy::Cartridge{test_rom(true, true)},
                                   gameboy::HardwareModel::sgb};
    check(!explicit_sgb.bus().cgb_mode() &&
              explicit_sgb.bus().read8(0xFF4F) == 0xFF,
          "explicit SGB selection keeps CGB-compatible software on the SGB path");
}

void test_public_model_catalog() {
    using gameboy::HardwareModel;
    constexpr std::array expected_catalog{
        HardwareModel::automatic, HardwareModel::dmg0, HardwareModel::dmg,
        HardwareModel::mgb, HardwareModel::sgb, HardwareModel::sgb2,
        HardwareModel::cgb0, HardwareModel::cgb_c, HardwareModel::cgb_e,
        HardwareModel::agb0, HardwareModel::agb};
    check(gameboy::selectable_hardware_models == expected_catalog,
          "public catalog preserves existing selections and appends AGB startup profiles");
    check(gameboy::concrete_hardware_models.size() == 8,
          "silicon-accuracy matrix retains its eight validated profiles");
    for (auto model : {HardwareModel::agb0, HardwareModel::agb}) {
        check(std::find(gameboy::concrete_hardware_models.begin(),
                        gameboy::concrete_hardware_models.end(), model) ==
                  gameboy::concrete_hardware_models.end(),
              "startup-only AGB profiles must not imply silicon-accuracy validation");
        check(gameboy::is_agb_hardware(model) && gameboy::is_cgb_hardware(model),
              "AGB startup profiles use the color-compatible runtime");
    }
    check(gameboy::hardware_model_id(HardwareModel::agb0) == "agb0" &&
              gameboy::hardware_model_id(HardwareModel::agb) == "agb" &&
              static_cast<int>(HardwareModel::cgb_e) == 9 &&
              static_cast<int>(HardwareModel::agb0) == 10 &&
              static_cast<int>(HardwareModel::agb) == 11,
          "AGB IDs are appended without changing persisted CGB revision IDs");
    check(gameboy::hardware_model_id(gameboy::HardwareModel::cgb_c) == "cgb-c" &&
              gameboy::hardware_model_id(gameboy::HardwareModel::cgb_e) == "cgb-e",
          "CGB revision IDs are stable for settings and runner reports");
    check(gameboy::hardware_model_name(gameboy::HardwareModel::sgb2) == "SGB2",
          "hardware model display names are stable");
}

} // namespace

int main() {
    try {
        test_explicit_profiles();
        test_cartridge_register_handoff();
        test_color_boot_mode_phases();
        test_power_on_work_ram();
        test_monochrome_header_logo();
        test_automatic_selection();
        test_public_model_catalog();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0) {
        std::cout << "All hardware model matrix contract tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
