#include "gameboy/cpu.hpp"
#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom(const std::vector<std::uint8_t>& program = {}) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "CPU CONTRACT";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    std::copy(program.begin(), program.end(), rom.begin() + 0x100);
    return rom;
}

gameboy::CpuRegisters initial_registers() {
    return {0x77, 0xB0, 0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0xFFFE, 0x0100};
}

std::uint8_t register_value(const gameboy::CpuRegisters& registers,
                            const unsigned index) {
    switch (index) {
    case 0: return registers.b;
    case 1: return registers.c;
    case 2: return registers.d;
    case 3: return registers.e;
    case 4: return registers.h;
    case 5: return registers.l;
    default: return registers.a;
    }
}

void test_state_normalization() {
    auto registers = initial_registers();
    registers.f = 0xFF;
    gameboy::Cpu cpu;
    cpu.load_registers(registers);
    check(cpu.registers().f == 0xF0, "CPU state masks unavailable flag bits");
}

void test_register_load_matrix() {
    const std::array<std::uint8_t, 8> values{
        0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0x66, 0x77,
    };
    for (unsigned destination = 0; destination < 8; ++destination) {
        for (unsigned source = 0; source < 8; ++source) {
            if (destination == 6 && source == 6) continue;
            const auto opcode = static_cast<std::uint8_t>(
                0x40 | (destination << 3) | source);
            gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({opcode})}};
            bus.write8(0xC000, values[6]);
            gameboy::Cpu cpu;
            cpu.load_registers(initial_registers());
            const auto cycles = cpu.step(bus);
            const auto label = "LD matrix opcode " + std::to_string(opcode);
            const auto actual = destination == 6
                                    ? bus.read8(0xC000)
                                    : register_value(cpu.registers(), destination);
            check(actual == values[source], label + " moves the correct value");
            check(cycles == (destination == 6 || source == 6 ? 8U : 4U),
                  label + " has the correct cycle count");
            check(cpu.registers().pc == 0x0101, label + " advances PC once");
            check(cpu.registers().f == 0xB0, label + " preserves flags");
        }
    }
}

void test_immediate_load_table() {
    struct LoadCase {
        std::uint8_t opcode;
        unsigned destination;
        unsigned cycles;
    };
    constexpr std::array<LoadCase, 8> cases{{
        {0x06, 0, 8}, {0x0E, 1, 8}, {0x16, 2, 8}, {0x1E, 3, 8},
        {0x26, 4, 8}, {0x2E, 5, 8}, {0x36, 6, 12}, {0x3E, 7, 8},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{
            gameboy::Cartridge{test_rom({test.opcode, 0xA5})}};
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        const auto cycles = cpu.step(bus);
        const auto actual = test.destination == 6
                                ? bus.read8(0xC000)
                                : register_value(cpu.registers(), test.destination);
        check(actual == 0xA5, "LD r,d8 writes its immediate operand");
        check(cycles == test.cycles, "LD r,d8 has the correct cycle count");
        check(cpu.registers().pc == 0x0102, "LD r,d8 consumes two bytes");
        check(cpu.registers().f == 0xB0, "LD r,d8 preserves flags");
    }
}

} // namespace

int main() {
    test_state_normalization();
    test_register_load_matrix();
    test_immediate_load_table();
    return failures == 0 ? 0 : 1;
}
