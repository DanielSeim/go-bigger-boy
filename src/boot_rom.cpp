#include "gameboy/boot_rom.hpp"

#include <cstddef>

namespace gameboy {
namespace {

struct HandoffRegisters {
    std::uint8_t a;
    std::uint8_t f;
    std::uint8_t b;
    std::uint8_t c;
    std::uint8_t d;
    std::uint8_t e;
    std::uint8_t h;
    std::uint8_t l;
};

HandoffRegisters handoff_registers(const HardwareModel model) noexcept {
    switch (model) {
    case HardwareModel::dmg0:
        return {0x01, 0x00, 0xFF, 0x13, 0x00, 0xC1, 0x84, 0x03};
    case HardwareModel::mgb:
        return {0xFF, 0xB0, 0x00, 0x13, 0x00, 0xD8, 0x01, 0x4D};
    case HardwareModel::sgb:
        return {0x01, 0x00, 0x00, 0x14, 0x00, 0x00, 0xC0, 0x60};
    case HardwareModel::sgb2:
        return {0xFF, 0x00, 0x00, 0x14, 0x00, 0x00, 0xC0, 0x60};
    case HardwareModel::cgb0:
    case HardwareModel::cgb:
    case HardwareModel::cgb_c:
    case HardwareModel::cgb_e:
        return {0x11, 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x7C};
    case HardwareModel::automatic:
    case HardwareModel::dmg:
        return {0x01, 0xB0, 0x00, 0x13, 0x00, 0xD8, 0x01, 0x4D};
    }
    return {0x01, 0xB0, 0x00, 0x13, 0x00, 0xD8, 0x01, 0x4D};
}

} // namespace

DiagnosticBootRom diagnostic_boot_rom(const HardwareModel model) noexcept {
    DiagnosticBootRom rom{};
    rom.fill(0x00); // NOP padding also makes the ROM easy to inspect in a dump.

    std::size_t offset = 0;
    const auto emit = [&rom, &offset](const std::uint8_t value) noexcept {
        if (offset < 0xFE) rom[offset++] = value;
    };
    const auto emit16 = [&emit](const std::uint16_t value) noexcept {
        emit(static_cast<std::uint8_t>(value));
        emit(static_cast<std::uint8_t>(value >> 8));
    };

    const auto registers = handoff_registers(model);
    const auto model_id = [&]() noexcept {
        switch (model) {
        case HardwareModel::dmg0: return std::uint8_t{0};
        case HardwareModel::dmg: return std::uint8_t{1};
        case HardwareModel::mgb: return std::uint8_t{2};
        case HardwareModel::sgb: return std::uint8_t{3};
        case HardwareModel::sgb2: return std::uint8_t{4};
        case HardwareModel::cgb0: return std::uint8_t{5};
        case HardwareModel::cgb_c: return std::uint8_t{6};
        case HardwareModel::cgb_e: return std::uint8_t{7};
        case HardwareModel::automatic:
        case HardwareModel::cgb: return std::uint8_t{8};
        }
        return std::uint8_t{1};
    }();

    // SP is the only stack-visible value required by this ROM. The marker is
    // deliberately in HRAM so cartridge RAM and mapper state remain untouched.
    emit(0x31); emit16(0xFFFE); // LD SP,$FFFE
    emit(0x3E); emit(0x47); emit(0xE0); emit(0x80); // FF80 = 'G'
    emit(0x3E); emit(0x42); emit(0xE0); emit(0x81); // FF81 = 'B'
    emit(0x3E); emit(0x42); emit(0xE0); emit(0x82); // FF82 = 'B'
    emit(0x3E); emit(model_id); emit(0xE0); emit(0x83); // FF83 = model
    emit(0x3E); emit(0x01); emit(0xE0); emit(0x84); // FF84 = BIOS version

    emit(0x3E); emit(registers.a); // LD A,a
    emit(0x06); emit(registers.b); // LD B,b
    emit(0x0E); emit(registers.c); // LD C,c
    emit(0x16); emit(registers.d); // LD D,d
    emit(0x1E); emit(registers.e); // LD E,e
    emit(0x26); emit(registers.h); // LD H,h
    emit(0x2E); emit(registers.l); // LD L,l

    // Recreate the post-boot flag register without a privileged register
    // write. These short sequences preserve the requested A value afterward.
    if (registers.f == 0xB0) {
        emit(0xE6); emit(0x00); // AND $00 -> ZH
        emit(0x2F);              // CPL -> ZNH
        emit(0xC6); emit(0x01); // ADD A,$01 -> ZHC
        emit(0x3E); emit(registers.a);
    } else if (registers.f == 0x80) {
        emit(0xAF);              // XOR A -> Z
        emit(0x3E); emit(registers.a);
    } else {
        emit(0xB7);              // OR A -> clear flags for nonzero A
    }

    emit(0xC3); emit16(0x00FE); // JP $00FE

    // Put the disable write at FE/FF. Once the instruction completes, the
    // next fetch is exactly the cartridge entry point at 0100.
    rom[0xFE] = 0xE0; // LDH [$FF50],A
    rom[0xFF] = 0x50;
    return rom;
}

} // namespace gameboy
