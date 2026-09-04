#include "gameboy/emulator.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::uint16_t program_address = 0x0100;
int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    const std::string title = "CORE TEST";
    for (std::size_t i = 0; i < title.size(); ++i) {
        rom[0x134 + i] = static_cast<std::uint8_t>(title[i]);
    }
    for (std::size_t i = 0; i < program.size(); ++i) {
        rom[program_address + i] = program[i];
    }
    return rom;
}

std::vector<std::uint8_t> cgb_test_rom(
    const std::vector<std::uint8_t>& program = {}) {
    auto rom = test_rom(program);
    rom[0x143] = 0x80;
    return rom;
}

void test_cgb_memory_and_rendering() {
    gameboy::Emulator speed{gameboy::Cartridge{cgb_test_rom({
        0x3E, 0x01, // LD A,1
        0xE0, 0x4D, // LDH (KEY1),A
        0x10, 0x00, // STOP: switch to double speed
        0x3E, 0x01, 0xE0, 0x4D, 0x10, 0x00,
    })}};
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    check(speed.bus().read8(0xFF4D) == 0x7F,
          "KEY1 exposes a requested CGB speed switch");
    static_cast<void>(speed.step());
    check(speed.bus().double_speed() && !speed.cpu().stopped() &&
              speed.bus().read8(0xFF4D) == 0xFE,
          "CGB STOP performs an armed double-speed switch");
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    static_cast<void>(speed.step());
    check(!speed.bus().double_speed() && !speed.cpu().stopped(),
          "a second armed STOP returns the CGB CPU to normal speed");

    gameboy::Emulator emulator{gameboy::Cartridge{cgb_test_rom()}};
    auto& bus = emulator.bus();
    check(bus.cgb_mode() && emulator.cpu().registers().a == 0x11 &&
              emulator.cpu().registers().f == 0x80 &&
              emulator.cpu().registers().e == 0x08 &&
              emulator.cpu().registers().l == 0x7C,
          "CGB cartridges start with CGB hardware detection state");
    auto edited_registers = emulator.cpu().registers();
    edited_registers.a = 0x42;
    edited_registers.f = 0xAF;
    edited_registers.sp = 0xC123;
    edited_registers.pc = 0x4567;
    emulator.set_cpu_registers(edited_registers);
    check(emulator.cpu().registers().a == 0x42 &&
              emulator.cpu().registers().f == 0xA0 &&
              emulator.cpu().registers().sp == 0xC123 &&
              emulator.cpu().registers().pc == 0x4567,
          "debugger register edits update CPU state and sanitize F");

    bus.write8(0xFF40, 0);
    bus.write8(0x8000, 0x12);
    bus.write8(0xFF4F, 1);
    bus.write8(0x8000, 0x34);
    check(bus.read8(0xFF4F) == 0xFF && bus.read8(0x8000) == 0x34,
          "VBK selects the second CGB VRAM bank");
    bus.write8(0xFF4F, 0);
    check(bus.read8(0x8000) == 0x12,
          "switching VBK restores the first CGB VRAM bank");
    bus.debug_write_vram(0, 0x0020, 0x56);
    bus.debug_write_vram(1, 0x0020, 0x78);
    check(bus.debug_read_vram(0, 0x0020) == 0x56 &&
              bus.debug_read_vram(1, 0x0020) == 0x78,
          "debugger VRAM access reads and edits either CGB bank directly");

    bus.write8(0xD000, 0x11);
    bus.write8(0xFF70, 2);
    bus.write8(0xD000, 0x22);
    bus.write8(0xFF70, 0);
    check(bus.read8(0xFF70) == 0xF9 && bus.read8(0xD000) == 0x11,
          "SVBK zero aliases bank one and preserves switched WRAM banks");
    bus.write8(0xFF70, 2);
    check(bus.read8(0xD000) == 0x22 && bus.read8(0xF000) == 0x22,
          "CGB switched WRAM is mirrored through echo RAM");

    bus.write8(0xFF4F, 1);
    bus.write8(0x8000, 0x80);
    bus.write8(0x8001, 0x00); // Bank 1 tile 0, first pixel color 1.
    bus.write8(0x9800, 0x08); // Tile attribute selects VRAM bank 1.
    bus.write8(0xFF4F, 0);
    bus.write8(0x9800, 0);
    bus.write8(0xFF68, 0x82); // Palette 0, color 1, auto-increment.
    bus.write8(0xFF69, 0x1F);
    bus.write8(0xFF69, 0x00); // RGB555 red.
    check(bus.read8(0xFF68) == 0xC4,
          "CGB palette writes auto-increment their six-bit index");
    bus.write8(0xFF40, 0x91);
    bus.tick(254);
    check(bus.framebuffer()[0] == 0xFFFF0000,
          "CGB tile attributes select VRAM banks and RGB555 palettes");

    bus.write8(0xFF40, 0);
    for (unsigned byte = 0; byte < 0x30; ++byte) {
        bus.write8(static_cast<std::uint16_t>(0xC000 + byte),
                   static_cast<std::uint8_t>(0x40 + byte));
    }
    bus.write8(0xFF4F, 1);
    bus.write8(0xFF51, 0xC0);
    bus.write8(0xFF52, 0x00);
    bus.write8(0xFF53, 0x01);
    bus.write8(0xFF54, 0x00);
    bus.write8(0xFF55, 0x00);
    check(bus.read8(0x8100) == 0x40 && bus.read8(0x810F) == 0x4F &&
              bus.read8(0xFF55) == 0xFF,
          "CGB general-purpose VRAM DMA copies complete 16-byte blocks");

    bus.write8(0xFF51, 0xC0);
    bus.write8(0xFF52, 0x10);
    bus.write8(0xFF53, 0x01);
    bus.write8(0xFF54, 0x20);
    bus.write8(0xFF40, 0x91);
    bus.write8(0xFF55, 0x81);
    bus.tick(254);
    check(bus.read8(0xFF55) == 0x00 && bus.read8(0x8120) == 0x50,
          "CGB HBlank DMA transfers one block at each HBlank");
    bus.tick(456);
    check(bus.read8(0xFF55) == 0xFF && bus.read8(0x8130) == 0x60,
          "CGB HBlank DMA completes after its requested block count");

    gameboy::MemoryBus batched_hdma{gameboy::Cartridge{cgb_test_rom()}};
    for (unsigned byte = 0; byte < 0x30; ++byte) {
        batched_hdma.write8(static_cast<std::uint16_t>(0xC000 + byte),
                            static_cast<std::uint8_t>(0x70 + byte));
    }
    batched_hdma.write8(0xFF51, 0xC0);
    batched_hdma.write8(0xFF52, 0x00);
    batched_hdma.write8(0xFF53, 0x01);
    batched_hdma.write8(0xFF54, 0x40);
    batched_hdma.write8(0xFF40, 0x91);
    batched_hdma.write8(0xFF55, 0x82); // Three HBlank blocks.
    batched_hdma.tick(720); // Crosses two HBlanks in one bus batch.
    check(batched_hdma.read8(0x8140) == 0x70 &&
              batched_hdma.read8(0x8150) == 0x80 &&
              batched_hdma.read8(0xFF55) == 0x00,
          "CGB HBlank DMA services every HBlank crossed by a batched tick");

    const auto cgb_state = emulator.save_state();
    bus.write8(0xFF40, 0);
    bus.write8(0xFF4F, 1);
    bus.write8(0x8100, 0);
    bus.write8(0xFF70, 2);
    bus.write8(0xD000, 0);
    emulator.load_state(cgb_state);
    check(bus.read8(0xFF4F) == 0xFF && bus.read8(0x8100) == 0x40 &&
              bus.read8(0xD000) == 0x22,
          "save states preserve CGB VRAM, WRAM, palettes, and bank selection");

    gameboy::MemoryBus dmg{gameboy::Cartridge{test_rom()}};
    check(!dmg.cgb_mode() && dmg.read8(0xFF4F) == 0xFF &&
              dmg.read8(0xFF68) == 0xFF && dmg.read8(0xFF70) == 0xFF,
          "CGB-only registers remain unavailable to monochrome cartridges");

    gameboy::Emulator compatibility{
        gameboy::Cartridge{test_rom()}, gameboy::HardwareModel::cgb};
    auto& compatibility_bus = compatibility.bus();
    compatibility_bus.write8(0xFF72, 0x12);
    compatibility_bus.write8(0xFF73, 0x34);
    compatibility_bus.write8(0xFF75, 0xFF);
    check(!compatibility_bus.cgb_mode() &&
              compatibility_bus.read8(0xFF4F) == 0xFE &&
              compatibility_bus.read8(0xFF68) == 0xC8 &&
              compatibility_bus.read8(0xFF69) == 0xFF &&
              compatibility_bus.read8(0xFF72) == 0x12 &&
              compatibility_bus.read8(0xFF73) == 0x34 &&
              compatibility_bus.read8(0xFF75) == 0xFF &&
              compatibility_bus.read8(0xFF76) == 0,
          "DMG software on CGB hardware exposes compatibility-mode registers");
}


} // namespace

int main() {
    try {
        test_cgb_memory_and_rendering();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        return 1;
    }

    if (failures == 0) {
        std::cout << "All CGB contract tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}

