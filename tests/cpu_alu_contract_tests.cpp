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
    constexpr std::string_view title = "ALU CONTRACT";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    std::copy(program.begin(), program.end(), rom.begin() + 0x100);
    return rom;
}

gameboy::CpuRegisters initial_registers() {
    return {0x77, 0xB0, 0x11, 0x22, 0x33, 0x44, 0xC0, 0x00, 0xFFFE, 0x0100};
}

void test_register_arithmetic() {
    struct ArithmeticCase {
        const char* name;
        std::uint8_t opcode;
        std::uint8_t a;
        std::uint8_t operand;
        std::uint8_t flags;
        std::uint8_t expected_a;
        std::uint8_t expected_flags;
    };
    constexpr std::array<ArithmeticCase, 12> cases{{
        {"ADD half carry", 0x80, 0x0F, 0x01, 0x00, 0x10, 0x20},
        {"ADD zero carry", 0x80, 0xFF, 0x01, 0x00, 0x00, 0xB0},
        {"ADC carry input", 0x88, 0x0F, 0x00, 0x10, 0x10, 0x20},
        {"ADC ignores stale flags", 0x88, 0x01, 0x01, 0xE0, 0x02, 0x00},
        {"SUB half borrow", 0x90, 0x10, 0x01, 0x00, 0x0F, 0x60},
        {"SUB full borrow", 0x90, 0x00, 0x01, 0x00, 0xFF, 0x70},
        {"SBC carry input", 0x98, 0x10, 0x00, 0x10, 0x0F, 0x60},
        {"SBC zero", 0x98, 0x01, 0x00, 0x10, 0x00, 0xC0},
        {"AND", 0xA0, 0xF0, 0x0F, 0xF0, 0x00, 0xA0},
        {"XOR", 0xA8, 0xAA, 0xAA, 0xF0, 0x00, 0x80},
        {"OR", 0xB0, 0x80, 0x01, 0xF0, 0x81, 0x00},
        {"CP", 0xB8, 0x10, 0x20, 0x00, 0x10, 0x50},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({test.opcode})}};
        auto registers = initial_registers();
        registers.a = test.a;
        registers.b = test.operand;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        const auto cycles = cpu.step(bus);
        check(cpu.registers().a == test.expected_a,
              std::string(test.name) + " produces the expected accumulator");
        check(cpu.registers().f == test.expected_flags,
              std::string(test.name) + " produces the expected flags");
        check(cpu.registers().pc == 0x0101,
              std::string(test.name) + " advances PC once");
        check(cycles == 4, std::string(test.name) + " takes four cycles");
    }
}

void test_immediate_arithmetic() {
    struct Case { std::uint8_t opcode, flags, expected_a, expected_flags; };
    constexpr std::array<Case, 8> cases{{
        {0xC6, 0x00, 0x11, 0x00}, {0xCE, 0x10, 0x12, 0x00},
        {0xD6, 0x00, 0x0F, 0x60}, {0xDE, 0x10, 0x0E, 0x60},
        {0xE6, 0xF0, 0x00, 0xA0}, {0xEE, 0xF0, 0x11, 0x00},
        {0xF6, 0xF0, 0x11, 0x00}, {0xFE, 0x00, 0x10, 0x60},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({test.opcode, 0x01})}};
        auto registers = initial_registers();
        registers.a = 0x10;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        const auto cycles = cpu.step(bus);
        check(cpu.registers().a == test.expected_a, "immediate ALU updates A");
        check(cpu.registers().f == test.expected_flags, "immediate ALU updates flags");
        check(cpu.registers().pc == 0x0102, "immediate ALU consumes two bytes");
        check(cycles == 8, "immediate ALU takes eight cycles");
    }
}

void test_memory_arithmetic() {
    struct Case { std::uint8_t opcode, a, operand, flags, expected_a, expected_flags; };
    constexpr std::array<Case, 8> cases{{
        {0x86, 0x01, 0x02, 0x00, 0x03, 0x00}, {0x8E, 0x01, 0x02, 0x10, 0x04, 0x00},
        {0x96, 0x03, 0x02, 0x00, 0x01, 0x40}, {0x9E, 0x03, 0x01, 0x10, 0x01, 0x40},
        {0xA6, 0xF0, 0x0F, 0x00, 0x00, 0xA0}, {0xAE, 0xAA, 0xAA, 0x00, 0x00, 0x80},
        {0xB6, 0x80, 0x01, 0x00, 0x81, 0x00}, {0xBE, 0x10, 0x20, 0x00, 0x10, 0x50},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({test.opcode})}};
        bus.write8(0xC000, test.operand);
        auto registers = initial_registers();
        registers.a = test.a;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        check(cpu.step(bus) == 8, "ALU A,(HL) takes eight cycles");
        check(cpu.registers().a == test.expected_a, "ALU A,(HL) updates A");
        check(cpu.registers().f == test.expected_flags, "ALU A,(HL) updates flags");
    }
}

void test_increment_decrement() {
    struct Case {
        const char* name;
        std::uint8_t opcode, input, flags, expected, expected_flags;
    };
    constexpr std::array<Case, 6> cases{{
        {"INC normal", 0x04, 0x01, 0x10, 0x02, 0x10},
        {"INC half carry", 0x04, 0x0F, 0x10, 0x10, 0x30},
        {"INC wrap", 0x04, 0xFF, 0x10, 0x00, 0xB0},
        {"DEC normal", 0x05, 0x02, 0x10, 0x01, 0x50},
        {"DEC half borrow", 0x05, 0x10, 0x10, 0x0F, 0x70},
        {"DEC zero", 0x05, 0x01, 0x10, 0x00, 0xD0},
    }};
    for (const auto& test : cases) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({test.opcode})}};
        auto registers = initial_registers();
        registers.b = test.input;
        registers.f = test.flags;
        gameboy::Cpu cpu;
        cpu.load_registers(registers);
        const auto cycles = cpu.step(bus);
        check(cpu.registers().b == test.expected,
              std::string(test.name) + " produces the expected value");
        check(cpu.registers().f == test.expected_flags,
              std::string(test.name) + " produces the expected flags");
        check(cycles == 4, std::string(test.name) + " takes four cycles");
    }
    for (const auto opcode : {std::uint8_t{0x34}, std::uint8_t{0x35}}) {
        gameboy::MemoryBus bus{gameboy::Cartridge{test_rom({opcode})}};
        bus.write8(0xC000, opcode == 0x34 ? 0x0F : 0x10);
        gameboy::Cpu cpu;
        cpu.load_registers(initial_registers());
        const auto cycles = cpu.step(bus);
        check(bus.read8(0xC000) == (opcode == 0x34 ? 0x10 : 0x0F),
              "INC/DEC (HL) updates memory");
        check(cycles == 12, "INC/DEC (HL) takes twelve cycles");
    }
}

} // namespace

int main() {
    test_register_arithmetic();
    test_immediate_arithmetic();
    test_memory_arithmetic();
    test_increment_decrement();
    return failures == 0 ? 0 : 1;
}
