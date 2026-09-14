#pragma once

#ifndef __ANDROID__

#include "gameboy/memory_bus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace gbb::sdl {

struct DisassembledInstruction {
    std::uint16_t address{};
    std::array<std::uint8_t, 3> bytes{};
    std::uint8_t length{1};
    std::string text;
};

namespace disassembler_detail {

inline std::string hex8(const std::uint8_t value) {
    std::ostringstream out;
    out << '$' << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << static_cast<unsigned>(value);
    return out.str();
}

inline std::string hex16(const std::uint16_t value) {
    std::ostringstream out;
    out << '$' << std::uppercase << std::hex << std::setfill('0')
        << std::setw(4) << value;
    return out.str();
}

inline std::string signed_offset(const std::uint8_t value) {
    const auto offset = value < 0x80 ? static_cast<int>(value)
                                    : static_cast<int>(value) - 0x100;
    if (offset < 0) return "-" + hex8(static_cast<std::uint8_t>(-offset));
    return "+" + hex8(static_cast<std::uint8_t>(offset));
}

inline const char* const register_names[] = {"B", "C", "D", "E",
                                               "H", "L", "(HL)", "A"};
inline const char* const pair_names[] = {"BC", "DE", "HL", "SP"};
inline const char* const stack_pair_names[] = {"BC", "DE", "HL", "AF"};
inline const char* const conditions[] = {"NZ", "Z", "NC", "C"};
inline const char* const alu_names[] = {"ADD A, ", "ADC A, ", "SUB ",
                                        "SBC A, ", "AND ", "XOR ",
                                        "OR ", "CP "};
inline const char* const rotate_names[] = {"RLC ", "RRC ", "RL ", "RR ",
                                           "SLA ", "SRA ", "SWAP ", "SRL "};

inline std::uint16_t word(const std::uint8_t low, const std::uint8_t high) {
    return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high)
                                             << 8U));
}

inline bool patterned_opcode(const std::uint8_t opcode,
                             const std::uint8_t mask,
                             const std::uint8_t value) {
    return (opcode & mask) == value;
}

inline DisassembledInstruction decode(const gameboy::MemoryBus& bus,
                                      const std::uint16_t address) {
    DisassembledInstruction instruction;
    instruction.address = address;
    instruction.bytes[0] = bus.read8(address);
    const auto opcode = instruction.bytes[0];
    const auto next = static_cast<std::uint16_t>(address + 1);
    const auto byte1 = [&] {
        instruction.bytes[1] = bus.read8(next);
        return instruction.bytes[1];
    };
    const auto byte2 = [&] {
        instruction.bytes[2] = bus.read8(static_cast<std::uint16_t>(next + 1));
        return instruction.bytes[2];
    };
    const auto imm16 = [&] {
        return word(byte1(), byte2());
    };
    const auto relative_target = [&] {
        const auto offset = byte1();
        const auto base = static_cast<std::uint16_t>(address + 2);
        return static_cast<std::uint16_t>(base +
                                          (offset < 0x80
                                               ? static_cast<int>(offset)
                                               : static_cast<int>(offset) - 0x100));
    };

    if (opcode == 0xCB) {
        const auto extended = byte1();
        instruction.length = 2;
        const auto group = static_cast<unsigned>(extended >> 6);
        const auto operation = static_cast<unsigned>((extended >> 3) & 7);
        const auto target = static_cast<unsigned>(extended & 7);
        if (group == 0) {
            instruction.text = std::string(rotate_names[operation]) +
                               register_names[target];
        } else if (group == 1) {
            instruction.text = "BIT " + std::to_string(operation) + "," +
                               register_names[target];
        } else if (group == 2) {
            instruction.text = "RES " + std::to_string(operation) + "," +
                               register_names[target];
        } else {
            instruction.text = "SET " + std::to_string(operation) + "," +
                               register_names[target];
        }
        return instruction;
    }

    if (opcode >= 0x40 && opcode <= 0x7F) {
        instruction.text = opcode == 0x76
                               ? "HALT"
                               : "LD " + std::string(register_names[(opcode >> 3) & 7]) +
                                     "," + register_names[opcode & 7];
        return instruction;
    }
    if (opcode >= 0x80 && opcode <= 0xBF) {
        instruction.text = std::string(alu_names[(opcode >> 3) & 7]) +
                           register_names[opcode & 7];
        return instruction;
    }
    if (patterned_opcode(opcode, 0xC7, 0x04)) {
        instruction.text = "INC " + std::string(register_names[(opcode >> 3) & 7]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xC7, 0x05)) {
        instruction.text = "DEC " + std::string(register_names[(opcode >> 3) & 7]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xC7, 0x06)) {
        instruction.length = 2;
        instruction.text = "LD " + std::string(register_names[(opcode >> 3) & 7]) +
                           "," + hex8(byte1());
        return instruction;
    }
    if (patterned_opcode(opcode, 0xCF, 0x01)) {
        instruction.length = 3;
        instruction.text = "LD " + std::string(pair_names[(opcode >> 4) & 3]) +
                           "," + hex16(imm16());
        return instruction;
    }
    if (patterned_opcode(opcode, 0xCF, 0x03)) {
        instruction.text = "INC " + std::string(pair_names[(opcode >> 4) & 3]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xCF, 0x0B)) {
        instruction.text = "DEC " + std::string(pair_names[(opcode >> 4) & 3]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xCF, 0x09)) {
        instruction.text = "ADD HL," + std::string(pair_names[(opcode >> 4) & 3]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xCF, 0xC1)) {
        instruction.text = "POP " +
                           std::string(stack_pair_names[(opcode >> 4) & 3]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xCF, 0xC5)) {
        instruction.text = "PUSH " +
                           std::string(stack_pair_names[(opcode >> 4) & 3]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xE7, 0x20)) {
        instruction.length = 2;
        instruction.text = "JR " + std::string(conditions[(opcode >> 3) & 3]) +
                           "," + hex16(relative_target());
        return instruction;
    }
    if (patterned_opcode(opcode, 0xE7, 0xC2)) {
        instruction.length = 3;
        instruction.text = "JP " + std::string(conditions[(opcode >> 3) & 3]) +
                           "," + hex16(imm16());
        return instruction;
    }
    if (patterned_opcode(opcode, 0xE7, 0xC4)) {
        instruction.length = 3;
        instruction.text = "CALL " + std::string(conditions[(opcode >> 3) & 3]) +
                           "," + hex16(imm16());
        return instruction;
    }
    if (patterned_opcode(opcode, 0xE7, 0xC0)) {
        instruction.text = "RET " + std::string(conditions[(opcode >> 3) & 3]);
        return instruction;
    }
    if (patterned_opcode(opcode, 0xC7, 0xC7)) {
        instruction.text = "RST " + hex8(opcode & 0x38);
        return instruction;
    }

    switch (opcode) {
    case 0x00: instruction.text = "NOP"; break;
    case 0x02: instruction.text = "LD (BC),A"; break;
    case 0x07: instruction.text = "RLCA"; break;
    case 0x08:
        instruction.length = 3;
        instruction.text = "LD (" + hex16(imm16()) + "),SP";
        break;
    case 0x0A: instruction.text = "LD A,(BC)"; break;
    case 0x0F: instruction.text = "RRCA"; break;
    case 0x10:
        instruction.length = 2;
        instruction.text = "STOP " + hex8(byte1());
        break;
    case 0x12: instruction.text = "LD (DE),A"; break;
    case 0x17: instruction.text = "RLA"; break;
    case 0x18:
        instruction.length = 2;
        instruction.text = "JR " + hex16(relative_target());
        break;
    case 0x1A: instruction.text = "LD A,(DE)"; break;
    case 0x1F: instruction.text = "RRA"; break;
    case 0x22: instruction.text = "LD (HL+),A"; break;
    case 0x27: instruction.text = "DAA"; break;
    case 0x2A: instruction.text = "LD A,(HL+)"; break;
    case 0x2F: instruction.text = "CPL"; break;
    case 0x32: instruction.text = "LD (HL-),A"; break;
    case 0x37: instruction.text = "SCF"; break;
    case 0x3A: instruction.text = "LD A,(HL-)"; break;
    case 0x3F: instruction.text = "CCF"; break;
    case 0x76: instruction.text = "HALT"; break;
    case 0xC3:
        instruction.length = 3;
        instruction.text = "JP " + hex16(imm16());
        break;
    case 0xC9: instruction.text = "RET"; break;
    case 0xCD:
        instruction.length = 3;
        instruction.text = "CALL " + hex16(imm16());
        break;
    case 0xD9: instruction.text = "RETI"; break;
    case 0xC6:
        instruction.length = 2;
        instruction.text = "ADD A," + hex8(byte1());
        break;
    case 0xCE:
        instruction.length = 2;
        instruction.text = "ADC A," + hex8(byte1());
        break;
    case 0xD6:
        instruction.length = 2;
        instruction.text = "SUB " + hex8(byte1());
        break;
    case 0xDE:
        instruction.length = 2;
        instruction.text = "SBC A," + hex8(byte1());
        break;
    case 0xE0:
        instruction.length = 2;
        instruction.text = "LDH ($FF00+" + hex8(byte1()) + "),A";
        break;
    case 0xE2: instruction.text = "LD (C),A"; break;
    case 0xE6:
        instruction.length = 2;
        instruction.text = "AND " + hex8(byte1());
        break;
    case 0xE8:
        instruction.length = 2;
        instruction.text = "ADD SP," + signed_offset(byte1());
        break;
    case 0xE9: instruction.text = "JP (HL)"; break;
    case 0xEA:
        instruction.length = 3;
        instruction.text = "LD (" + hex16(imm16()) + "),A";
        break;
    case 0xEE:
        instruction.length = 2;
        instruction.text = "XOR " + hex8(byte1());
        break;
    case 0xF0:
        instruction.length = 2;
        instruction.text = "LDH A,($FF00+" + hex8(byte1()) + ")";
        break;
    case 0xF2: instruction.text = "LD A,(C)"; break;
    case 0xF3: instruction.text = "DI"; break;
    case 0xF6:
        instruction.length = 2;
        instruction.text = "OR " + hex8(byte1());
        break;
    case 0xF8:
        instruction.length = 2;
        instruction.text = "LD HL,SP" + signed_offset(byte1());
        break;
    case 0xF9: instruction.text = "LD SP,HL"; break;
    case 0xFA:
        instruction.length = 3;
        instruction.text = "LD A,(" + hex16(imm16()) + ")";
        break;
    case 0xFB: instruction.text = "EI"; break;
    case 0xFE:
        instruction.length = 2;
        instruction.text = "CP " + hex8(byte1());
        break;
    default:
        instruction.text = "DB " + hex8(opcode);
        break;
    }
    return instruction;
}

} // namespace disassembler_detail

inline DisassembledInstruction disassemble_instruction(
    const gameboy::MemoryBus& bus, const std::uint16_t address) {
    return disassembler_detail::decode(bus, address);
}

inline std::vector<DisassembledInstruction> disassemble(
    const gameboy::MemoryBus& bus, std::uint16_t address,
    const std::size_t count) {
    std::vector<DisassembledInstruction> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto instruction = disassemble_instruction(bus, address);
        result.push_back(instruction);
        address = static_cast<std::uint16_t>(address + instruction.length);
    }
    return result;
}

inline std::string disassembly_bytes(const DisassembledInstruction& instruction) {
    std::ostringstream out;
    for (unsigned index = 0; index < instruction.length; ++index) {
        if (index != 0) out << ' ';
        out << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned>(instruction.bytes[index]);
    }
    return out.str();
}

} // namespace gbb::sdl

#endif
