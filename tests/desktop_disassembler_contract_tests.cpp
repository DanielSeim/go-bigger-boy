#include "desktop_disassembler.hpp"

#include "gameboy/cartridge.hpp"
#include "gameboy/memory_bus.hpp"

#include <array>
#include <cstdint>
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

gameboy::MemoryBus test_bus(const std::vector<std::uint8_t>& program) {
    std::vector<std::uint8_t> rom(0x8000, 0);
    for (std::size_t index = 0; index < program.size(); ++index) {
        rom[0x100 + index] = program[index];
    }
    // The opcode sweep intentionally fills the area after $0100, which also
    // includes cartridge header bytes. Restore the size/type fields so the
    // synthetic cartridge remains a valid ROM.
    rom[0x147] = 0x00;
    rom[0x148] = 0x00;
    rom[0x149] = 0x00;
    return gameboy::MemoryBus{gameboy::Cartridge{std::move(rom)}};
}

void test_common_instructions() {
    auto bus = test_bus({0x01, 0x34, 0x12, 0x20, 0xFE, 0xCB, 0x7C,
                         0xD3, 0x00});
    const auto load = gbb::sdl::disassemble_instruction(bus, 0x0100);
    check(load.length == 3 && load.text == "LD BC,$1234",
          "disassembles a sixteen-bit immediate load");
    check(gbb::sdl::disassembly_bytes(load) == "01 34 12",
          "formats instruction bytes in display order");

    const auto jump = gbb::sdl::disassemble_instruction(bus, 0x0103);
    check(jump.length == 2 && jump.text == "JR NZ,$0103",
          "renders relative branches as absolute targets");

    const auto bit = gbb::sdl::disassemble_instruction(bus, 0x0105);
    check(bit.length == 2 && bit.text == "BIT 7,H",
          "decodes CB-prefixed bit operations");

    const auto invalid = gbb::sdl::disassemble_instruction(bus, 0x0107);
    check(invalid.length == 1 && invalid.text == "DB $D3",
          "marks unused opcodes as data instead of inventing an instruction");
}

void test_sequence_walk() {
    auto bus = test_bus({0x3E, 0x42, 0xEA, 0x00, 0xC0, 0x00});
    const auto lines = gbb::sdl::disassemble(bus, 0x0100, 3);
    check(lines.size() == 3, "returns the requested number of instructions");
    check(lines[0].address == 0x0100 && lines[1].address == 0x0102 &&
              lines[2].address == 0x0105,
          "walks by decoded instruction length");
    check(lines[1].text == "LD ($C000),A" && lines[2].text == "NOP",
          "keeps decoding after a three-byte instruction");
}

void test_all_opcodes_are_safe() {
    std::vector<std::uint8_t> program(0x300, 0);
    for (unsigned opcode = 0; opcode < 0x100; ++opcode) {
        program[opcode * 3] = static_cast<std::uint8_t>(opcode);
        program[opcode * 3 + 1] = 0x12;
        program[opcode * 3 + 2] = 0x34;
    }
    auto bus = test_bus(program);
    for (unsigned opcode = 0; opcode < 0x100; ++opcode) {
        const auto instruction = gbb::sdl::disassemble_instruction(
            bus, static_cast<std::uint16_t>(0x0100 + opcode * 3));
        check(instruction.length >= 1 && instruction.length <= 3 &&
                  !instruction.text.empty(),
              "every opcode has a bounded disassembly");
    }
}

} // namespace

int main() {
    test_common_instructions();
    test_sequence_walk();
    test_all_opcodes_are_safe();
    return failures == 0 ? 0 : 1;
}
