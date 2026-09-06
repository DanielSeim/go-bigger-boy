#include "gameboy/emulator.hpp"
#include "gameboy/hardware_model.hpp"

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

constexpr std::array<ModelCase, 7> model_cases{{
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
     {0x11, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x7C}, 0x28, 0, true,
     false, true},
    {gameboy::HardwareModel::cgb, "CGB",
     {0x11, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x7C}, 0x26, 0, true,
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
}

} // namespace

int main() {
    try {
        test_explicit_profiles();
        test_automatic_selection();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures == 0) {
        std::cout << "All hardware model matrix contract tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
