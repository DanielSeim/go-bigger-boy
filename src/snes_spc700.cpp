#include "gameboy/snes_spc700.hpp"

#include "gameboy/snes_audio_host.hpp"

namespace gameboy {
namespace {
constexpr std::uint8_t negative = 0x80;
constexpr std::uint8_t page = 0x20;
constexpr std::uint8_t zero = 0x02;
constexpr std::uint8_t carry = 0x01;
constexpr std::uint8_t half_carry = 0x08;
constexpr std::uint8_t overflow = 0x40;
} // namespace

std::uint8_t SnesSpc700::fetch() noexcept {
    const auto value = bus_.spc_read(registers_.pc);
    ++registers_.pc;
    return value;
}

std::uint16_t SnesSpc700::direct_address(const std::uint8_t offset) const noexcept {
    return static_cast<std::uint16_t>(
        ((registers_.psw & page) != 0 ? 0x100U : 0U) | offset);
}

std::uint8_t SnesSpc700::read_direct(const std::uint8_t offset) noexcept {
    return bus_.spc_read(direct_address(offset));
}

void SnesSpc700::write_direct(const std::uint8_t offset,
                              const std::uint8_t value) noexcept {
    bus_.spc_write(direct_address(offset), value);
}

void SnesSpc700::set_nz8(const std::uint8_t value) noexcept {
    registers_.psw = static_cast<std::uint8_t>(
        (registers_.psw & ~(negative | zero)) |
        (value & negative) | (value == 0 ? zero : 0));
}

void SnesSpc700::set_nz16(const std::uint16_t value) noexcept {
    registers_.psw = static_cast<std::uint8_t>(
        (registers_.psw & ~(negative | zero)) |
        ((value >> 8) & negative) | (value == 0 ? zero : 0));
}

void SnesSpc700::compare(const std::uint8_t lhs,
                         const std::uint8_t rhs) noexcept {
    set_nz8(static_cast<std::uint8_t>(lhs - rhs));
    registers_.psw = static_cast<std::uint8_t>(
        (registers_.psw & ~carry) | (lhs >= rhs ? carry : 0));
}

void SnesSpc700::add_with_carry(const std::uint8_t rhs) noexcept {
    const auto lhs = registers_.a;
    const auto carry_in = (registers_.psw & carry) != 0 ? 1U : 0U;
    const auto result = static_cast<unsigned>(lhs) + rhs + carry_in;
    const auto value = static_cast<std::uint8_t>(result);
    registers_.psw = static_cast<std::uint8_t>(
        (registers_.psw & ~(carry | half_carry | overflow)) |
        (result > 0xFFU ? carry : 0) |
        (((lhs & 0x0FU) + (rhs & 0x0FU) + carry_in) > 0x0FU ?
            half_carry : 0) |
        ((~(lhs ^ rhs) & (lhs ^ value) & 0x80U) != 0 ? overflow : 0));
    registers_.a = value;
    set_nz8(value);
}

void SnesSpc700::subtract_with_carry(const std::uint8_t rhs) noexcept {
    const auto lhs = registers_.a;
    const auto borrow = (registers_.psw & carry) != 0 ? 0U : 1U;
    const auto result = static_cast<unsigned>(lhs) - rhs - borrow;
    const auto value = static_cast<std::uint8_t>(result);
    registers_.psw = static_cast<std::uint8_t>(
        (registers_.psw & ~(carry | half_carry | overflow)) |
        (result <= 0xFFU ? carry : 0) |
        ((lhs & 0x0FU) >= (rhs & 0x0FU) + borrow ? half_carry : 0) |
        (((lhs ^ rhs) & (lhs ^ value) & 0x80U) != 0 ? overflow : 0));
    registers_.a = value;
    set_nz8(value);
}

unsigned SnesSpc700::branch(const bool take) noexcept {
    const auto displacement = static_cast<std::int8_t>(fetch());
    if (take) {
        registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
    }
    return take ? 4U : 2U;
}

void SnesSpc700::push(const std::uint8_t value) noexcept {
    bus_.spc_write(static_cast<std::uint16_t>(0x100U | registers_.sp), value);
    --registers_.sp;
}

std::uint8_t SnesSpc700::pop() noexcept {
    return bus_.spc_read(static_cast<std::uint16_t>(0x100U | ++registers_.sp));
}

SnesSpc700::StepResult SnesSpc700::step() noexcept {
    const auto start = registers_.pc;
    const auto opcode = fetch();
    unsigned cycles = 0;
    switch (opcode) {
    case 0x00: cycles = 2; break; // NOP
    case 0x20: registers_.psw &= static_cast<std::uint8_t>(~page);
               cycles = 2; break; // CLRP: select direct page zero
    case 0x80: registers_.psw |= carry; cycles = 2; break; // SETC
    case 0x60: registers_.psw &= static_cast<std::uint8_t>(~carry);
               cycles = 2; break; // CLRC
    case 0xED: registers_.psw ^= carry; cycles = 3; break; // NOTC
    case 0x40: registers_.psw |= page;
               cycles = 2; break; // SETP: select direct page one
    case 0xCD: registers_.x = fetch(); set_nz8(registers_.x); cycles = 2; break;
    case 0x7D: registers_.a = registers_.x; set_nz8(registers_.a);
               cycles = 2; break; // MOV A,X
    case 0xBD: registers_.sp = registers_.x; cycles = 2; break;
    case 0xE8: registers_.a = fetch(); set_nz8(registers_.a); cycles = 2; break;
    case 0xF4: { // MOV A,dp+X; direct-page offset wraps before page select
        const auto offset = static_cast<std::uint8_t>(fetch() + registers_.x);
        registers_.a = read_direct(offset);
        set_nz8(registers_.a);
        cycles = 4;
        break;
    }
    case 0x8D: registers_.y = fetch(); set_nz8(registers_.y); cycles = 2; break;
    case 0x28: registers_.a &= fetch(); set_nz8(registers_.a);
               cycles = 2; break; // AND A,#imm
    case 0x24: registers_.a &= read_direct(fetch()); set_nz8(registers_.a);
               cycles = 3; break; // AND A,dp
    case 0x08: registers_.a |= fetch(); set_nz8(registers_.a);
               cycles = 2; break; // OR A,#imm
    case 0x04: registers_.a |= read_direct(fetch()); set_nz8(registers_.a);
               cycles = 3; break; // OR A,dp
    case 0x18: { // OR dp,#imm: immediate is encoded first
        const auto immediate = fetch();
        const auto offset = fetch();
        const auto value = static_cast<std::uint8_t>(
            read_direct(offset) | immediate);
        write_direct(offset, value);
        set_nz8(value);
        cycles = 5;
        break;
    }
    case 0x09: { // OR dp,dp: source offset precedes destination
        const auto source = fetch();
        const auto destination = fetch();
        const auto value = static_cast<std::uint8_t>(
            read_direct(destination) | read_direct(source));
        write_direct(destination, value);
        set_nz8(value);
        cycles = 6;
        break;
    }
    case 0x38: { // AND dp,#imm: immediate is encoded first
        const auto immediate = fetch();
        const auto offset = fetch();
        const auto value = static_cast<std::uint8_t>(
            read_direct(offset) & immediate);
        write_direct(offset, value);
        set_nz8(value);
        cycles = 5;
        break;
    }
    case 0x88: add_with_carry(fetch()); cycles = 2; break; // ADC A,#imm
    case 0xA8: subtract_with_carry(fetch()); cycles = 2; break; // SBC A,#imm
    case 0xB5: { // SBC A,!abs+X
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.x);
        subtract_with_carry(bus_.spc_read(address));
        cycles = 5;
        break;
    }
    case 0xB6: { // SBC A,!abs+Y
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.y);
        subtract_with_carry(bus_.spc_read(address));
        cycles = 5;
        break;
    }
    case 0x98: { // ADC dp,#imm: immediate precedes destination
        const auto immediate = fetch();
        const auto offset = fetch();
        const auto saved_a = registers_.a;
        registers_.a = read_direct(offset);
        add_with_carry(immediate);
        write_direct(offset, registers_.a);
        registers_.a = saved_a;
        cycles = 5;
        break;
    }
    case 0x84: add_with_carry(read_direct(fetch()));
               cycles = 3; break; // ADC A,dp
    case 0x95: { // ADC A,!abs+X
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.x);
        add_with_carry(bus_.spc_read(address));
        cycles = 5;
        break;
    }
    case 0x85: { // ADC A,!abs
        const auto low = fetch();
        const auto high = fetch();
        add_with_carry(bus_.spc_read(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8))));
        cycles = 4;
        break;
    }
    case 0x96: { // ADC A,!abs+Y
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.y);
        add_with_carry(bus_.spc_read(address));
        cycles = 5;
        break;
    }
    case 0x48: registers_.a ^= fetch(); set_nz8(registers_.a);
               cycles = 2; break; // EOR A,#imm
    case 0x44: registers_.a ^= read_direct(fetch()); set_nz8(registers_.a);
               cycles = 3; break; // EOR A,dp
    case 0x1C: { // ASL A
        const auto value = registers_.a;
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | ((value & 0x80U) != 0 ? carry : 0));
        registers_.a = static_cast<std::uint8_t>(value << 1);
        set_nz8(registers_.a);
        cycles = 2;
        break;
    }
    case 0x9F: // XCN A
        registers_.a = static_cast<std::uint8_t>(
            (registers_.a << 4) | (registers_.a >> 4));
        set_nz8(registers_.a);
        cycles = 5;
        break;
    case 0x0B: { // ASL dp
        const auto offset = fetch();
        const auto old = read_direct(offset);
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | ((old & 0x80U) != 0 ? carry : 0));
        const auto value = static_cast<std::uint8_t>(old << 1);
        write_direct(offset, value);
        set_nz8(value);
        cycles = 4;
        break;
    }
    case 0x0E: { // TSET1 !abs: N/Z from A - old memory, then set bits
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        const auto old = bus_.spc_read(address);
        set_nz8(static_cast<std::uint8_t>(registers_.a - old));
        bus_.spc_write(address, static_cast<std::uint8_t>(old | registers_.a));
        cycles = 6;
        break;
    }
    case 0x4E: { // TCLR1 !abs: N/Z from A - old memory, then clear bits
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        const auto old = bus_.spc_read(address);
        set_nz8(static_cast<std::uint8_t>(registers_.a - old));
        bus_.spc_write(address, static_cast<std::uint8_t>(old & ~registers_.a));
        cycles = 6;
        break;
    }
    case 0x5C: { // LSR A
        const auto value = registers_.a;
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | (value & 1U));
        registers_.a = static_cast<std::uint8_t>(value >> 1);
        set_nz8(registers_.a);
        cycles = 2;
        break;
    }
    case 0x4B: { // LSR dp
        const auto offset = fetch();
        const auto old = read_direct(offset);
        const auto value = static_cast<std::uint8_t>(old >> 1);
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | (old & 1U));
        write_direct(offset, value);
        set_nz8(value);
        cycles = 4;
        break;
    }
    case 0x4C: { // LSR !abs
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        const auto old = bus_.spc_read(address);
        const auto value = static_cast<std::uint8_t>(old >> 1);
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | (old & 1U));
        bus_.spc_write(address, value);
        set_nz8(value);
        cycles = 5;
        break;
    }
    case 0x2B: { // ROL dp
        const auto offset = fetch();
        const auto old = read_direct(offset);
        const auto value = static_cast<std::uint8_t>(
            (old << 1) | ((registers_.psw & carry) != 0 ? 1U : 0U));
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | ((old & 0x80U) != 0 ? carry : 0));
        write_direct(offset, value);
        set_nz8(value);
        cycles = 4;
        break;
    }
    case 0x6B: { // ROR dp
        const auto offset = fetch();
        const auto old = read_direct(offset);
        const auto value = static_cast<std::uint8_t>(
            (old >> 1) | ((registers_.psw & carry) != 0 ? 0x80U : 0U));
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | (old & 1U));
        write_direct(offset, value);
        set_nz8(value);
        cycles = 4;
        break;
    }
    case 0x7C: { // ROR A
        const auto old = registers_.a;
        registers_.a = static_cast<std::uint8_t>(
            (old >> 1) | ((registers_.psw & carry) != 0 ? 0x80U : 0U));
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | (old & 1U));
        set_nz8(registers_.a);
        cycles = 2;
        break;
    }
    case 0xC6: write_direct(registers_.x, registers_.a); cycles = 4; break;
    case 0xD4: { // MOV dp+X,A
        const auto offset = static_cast<std::uint8_t>(fetch() + registers_.x);
        write_direct(offset, registers_.a);
        cycles = 5;
        break;
    }
    case 0xD8: write_direct(fetch(), registers_.x); cycles = 4; break; // MOV dp,X
    case 0xC9: { // MOV !abs,X
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        bus_.spc_write(address, registers_.x);
        cycles = 5;
        break;
    }
    case 0xAF: write_direct(registers_.x++, registers_.a); cycles = 4; break;
    case 0x1D: --registers_.x; set_nz8(registers_.x); cycles = 2; break;
    case 0x3D: ++registers_.x; set_nz8(registers_.x); cycles = 2; break;
    case 0xBC: ++registers_.a; set_nz8(registers_.a); cycles = 2; break;
    case 0xC8: compare(registers_.x, fetch()); cycles = 2; break; // CMP X,#imm
    case 0x68: compare(registers_.a, fetch()); cycles = 2; break; // CMP A,#imm
    case 0x75: { // CMP A,!abs+X
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        compare(registers_.a, bus_.spc_read(static_cast<std::uint16_t>(
            address + registers_.x)));
        cycles = 5;
        break;
    }
    case 0xAD: compare(registers_.y, fetch()); cycles = 2; break; // CMP Y,#imm
    case 0x5E: { // CMP Y,!abs
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        compare(registers_.y, bus_.spc_read(address));
        cycles = 4;
        break;
    }
    case 0x3F: { // CALL !abs
        const auto low = fetch();
        const auto high = fetch();
        const auto target = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        push(static_cast<std::uint8_t>(registers_.pc >> 8));
        push(static_cast<std::uint8_t>(registers_.pc));
        registers_.pc = target;
        cycles = 8;
        break;
    }
    case 0x6D: push(registers_.y); cycles = 4; break; // PUSH Y
    case 0x4D: push(registers_.x); cycles = 4; break; // PUSH X
    case 0x2D: push(registers_.a); cycles = 4; break; // PUSH A
    case 0xEE: registers_.y = pop(); cycles = 4; break; // POP Y, flags unchanged
    case 0xCE: registers_.x = pop(); cycles = 4; break; // POP X, flags unchanged
    case 0xAE: registers_.a = pop(); cycles = 4; break; // POP A, flags unchanged
    case 0x6F: { // RET
        const auto low = pop();
        const auto high = pop();
        registers_.pc = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        cycles = 5;
        break;
    }
    case 0x5F: { // JMP !abs
        const auto low = fetch();
        const auto high = fetch();
        registers_.pc = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        cycles = 3;
        break;
    }
    case 0xCF: { // MUL YA: N/Z reflect the high byte only
        const auto result = static_cast<std::uint16_t>(
            registers_.y * registers_.a);
        registers_.a = static_cast<std::uint8_t>(result);
        registers_.y = static_cast<std::uint8_t>(result >> 8);
        set_nz8(registers_.y);
        cycles = 9;
        break;
    }
    case 0x9E: { // DIV YA,X; 9-bit restoring divider, including overflow cases
        std::uint32_t result = static_cast<std::uint32_t>(
            registers_.a | (static_cast<unsigned>(registers_.y) << 8));
        const auto divisor = static_cast<std::uint32_t>(registers_.x) << 9;
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~(half_carry | overflow)) |
            ((registers_.y & 0x0FU) >= (registers_.x & 0x0FU)
                ? half_carry : 0));
        for (unsigned bit = 0; bit < 9; ++bit) {
            result <<= 1;
            if ((result & 0x20000U) != 0)
                result = (result & 0x1FFFFU) | 1U;
            if (result >= divisor) result ^= 1U;
            if ((result & 1U) != 0)
                result = (result - divisor) & 0x1FFFFU;
        }
        if ((result & 0x100U) != 0) registers_.psw |= overflow;
        registers_.a = static_cast<std::uint8_t>(result);
        registers_.y = static_cast<std::uint8_t>(result >> 9);
        set_nz8(registers_.a);
        cycles = 12;
        break;
    }
    case 0x9A: { // SUBW YA,dp; H is from the high-byte subtraction
        const auto offset = fetch();
        const auto rhs = static_cast<unsigned>(read_direct(offset)) |
            (static_cast<unsigned>(read_direct(
                static_cast<std::uint8_t>(offset + 1))) << 8);
        const auto lhs = static_cast<unsigned>(registers_.a) |
            (static_cast<unsigned>(registers_.y) << 8);
        const auto result = static_cast<std::uint16_t>(lhs - rhs);
        const auto low_borrow = (lhs & 0xFFU) < (rhs & 0xFFU) ? 1U : 0U;
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~(carry | half_carry | overflow)) |
            (lhs >= rhs ? carry : 0) |
            (((lhs >> 8) & 0x0FU) >=
                (((rhs >> 8) & 0x0FU) + low_borrow) ? half_carry : 0) |
            (((lhs ^ rhs) & (lhs ^ result) & 0x8000U) != 0
                ? overflow : 0));
        registers_.a = static_cast<std::uint8_t>(result);
        registers_.y = static_cast<std::uint8_t>(result >> 8);
        set_nz16(result);
        cycles = 5;
        break;
    }
    case 0x7A: { // ADDW YA,dp; H is from the high-byte addition
        const auto offset = fetch();
        const auto rhs = static_cast<unsigned>(read_direct(offset)) |
            (static_cast<unsigned>(read_direct(
                static_cast<std::uint8_t>(offset + 1))) << 8);
        const auto lhs = static_cast<unsigned>(registers_.a) |
            (static_cast<unsigned>(registers_.y) << 8);
        const auto sum = lhs + rhs;
        const auto result = static_cast<std::uint16_t>(sum);
        const auto low_carry = ((lhs & 0xFFU) + (rhs & 0xFFU)) > 0xFFU
            ? 1U : 0U;
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~(carry | half_carry | overflow)) |
            (sum > 0xFFFFU ? carry : 0) |
            ((((lhs >> 8) & 0x0FU) + ((rhs >> 8) & 0x0FU) +
                low_carry) > 0x0FU ? half_carry : 0) |
            ((~(lhs ^ rhs) & (lhs ^ result) & 0x8000U) != 0
                ? overflow : 0));
        registers_.a = static_cast<std::uint8_t>(result);
        registers_.y = static_cast<std::uint8_t>(result >> 8);
        set_nz16(result);
        cycles = 5;
        break;
    }
    case 0xD0: cycles = branch((registers_.psw & zero) == 0); break;
    case 0xF0: cycles = branch((registers_.psw & zero) != 0); break;
    case 0x10: cycles = branch((registers_.psw & negative) == 0); break;
    case 0x30: cycles = branch((registers_.psw & negative) != 0); break; // BMI
    case 0xB0: cycles = branch((registers_.psw & carry) != 0); break; // BCS
    case 0x90: cycles = branch((registers_.psw & carry) == 0); break; // BCC
    case 0x2F: cycles = branch(true); break;
    case 0xFE: { // DBNZ Y,rel: flags are unchanged
        const auto displacement = static_cast<std::int8_t>(fetch());
        --registers_.y;
        if (registers_.y != 0) {
            registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
            cycles = 6;
        } else {
            cycles = 4;
        }
        break;
    }
    case 0x6E: { // DBNZ dp,rel: flags are unchanged
        const auto offset = fetch();
        const auto displacement = static_cast<std::int8_t>(fetch());
        const auto value = static_cast<std::uint8_t>(read_direct(offset) - 1U);
        write_direct(offset, value);
        if (value != 0) {
            registers_.pc = static_cast<std::uint16_t>(
                registers_.pc + displacement);
            cycles = 7;
        } else {
            cycles = 5;
        }
        break;
    }
    case 0xDE: { // CBNE dp+X,rel: compare without changing flags
        const auto offset = static_cast<std::uint8_t>(fetch() + registers_.x);
        const auto displacement = static_cast<std::int8_t>(fetch());
        const auto take = registers_.a != read_direct(offset);
        if (take)
            registers_.pc = static_cast<std::uint16_t>(
                registers_.pc + displacement);
        cycles = take ? 8 : 6;
        break;
    }
    case 0x8F: {
        const auto value = fetch();
        write_direct(fetch(), value);
        cycles = 5;
        break;
    }
    case 0x78: {
        const auto value = fetch();
        compare(read_direct(fetch()), value);
        cycles = 5;
        break;
    }
    case 0x64: compare(registers_.a, read_direct(fetch())); cycles = 3; break;
    case 0x69: { // CMP dp,dp: encoded source offset, then destination
        const auto source = fetch();
        const auto destination = fetch();
        const auto source_value = read_direct(source);
        const auto destination_value = read_direct(destination);
        compare(destination_value, source_value);
        cycles = 6;
        break;
    }
    case 0xEB: registers_.y = read_direct(fetch()); set_nz8(registers_.y); cycles = 3; break;
    case 0xFB: registers_.y = read_direct(static_cast<std::uint8_t>(
                   fetch() + registers_.x));
               set_nz8(registers_.y); cycles = 4; break; // MOV Y,dp+X
    case 0xF8: registers_.x = read_direct(fetch()); set_nz8(registers_.x); cycles = 3; break;
    case 0x7E: compare(registers_.y, read_direct(fetch())); cycles = 3; break;
    case 0xE4: registers_.a = read_direct(fetch()); set_nz8(registers_.a); cycles = 3; break;
    case 0xE6: registers_.a = read_direct(registers_.x);
               set_nz8(registers_.a); cycles = 3; break; // MOV A,(X)
    case 0xE7: { // MOV A,[dp+X]
        const auto offset = static_cast<std::uint8_t>(fetch() + registers_.x);
        const auto lo = read_direct(offset);
        const auto hi = read_direct(static_cast<std::uint8_t>(offset + 1));
        registers_.a = bus_.spc_read(static_cast<std::uint16_t>(
            lo | (static_cast<unsigned>(hi) << 8)));
        set_nz8(registers_.a);
        cycles = 6;
        break;
    }
    case 0xE5: { // MOV A,!abs
        const auto low = fetch();
        const auto high = fetch();
        registers_.a = bus_.spc_read(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)));
        set_nz8(registers_.a);
        cycles = 4;
        break;
    }
    case 0xEC: { // MOV Y,!abs
        const auto low = fetch();
        const auto high = fetch();
        registers_.y = bus_.spc_read(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)));
        set_nz8(registers_.y);
        cycles = 4;
        break;
    }
    case 0xE9: { // MOV X,!abs
        const auto low = fetch();
        const auto high = fetch();
        registers_.x = bus_.spc_read(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)));
        set_nz8(registers_.x);
        cycles = 4;
        break;
    }
    case 0xF6: { // MOV A,!abs+Y
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.y);
        registers_.a = bus_.spc_read(address);
        set_nz8(registers_.a);
        cycles = 5;
        break;
    }
    case 0xF5: { // MOV A,!abs+X
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.x);
        registers_.a = bus_.spc_read(address);
        set_nz8(registers_.a);
        cycles = 5;
        break;
    }
    case 0xCB: write_direct(fetch(), registers_.y); cycles = 4; break;
    case 0xD6: { // MOV !abs+Y,A
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.y);
        bus_.spc_write(address, registers_.a);
        cycles = 6;
        break;
    }
    case 0xD7: {
        const auto offset = fetch();
        const auto lo = read_direct(offset);
        const auto hi = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto address = static_cast<std::uint16_t>(
            (lo | (static_cast<unsigned>(hi) << 8)) + registers_.y);
        bus_.spc_write(address, registers_.a);
        cycles = 7;
        break;
    }
    case 0xF7: { // MOV A,[dp]+Y
        const auto offset = fetch();
        const auto lo = read_direct(offset);
        const auto hi = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto address = static_cast<std::uint16_t>(
            (lo | (static_cast<unsigned>(hi) << 8)) + registers_.y);
        registers_.a = bus_.spc_read(address);
        set_nz8(registers_.a);
        cycles = 6;
        break;
    }
    case 0xFC: ++registers_.y; set_nz8(registers_.y); cycles = 2; break;
    case 0xDC: --registers_.y; set_nz8(registers_.y); cycles = 2; break; // DEC Y
    case 0xAB: {
        const auto offset = fetch();
        const auto value = static_cast<std::uint8_t>(read_direct(offset) + 1);
        write_direct(offset, value);
        set_nz8(value);
        cycles = 4;
        break;
    }
    case 0x8B: { // DEC dp
        const auto offset = fetch();
        const auto value = static_cast<std::uint8_t>(read_direct(offset) - 1U);
        write_direct(offset, value);
        set_nz8(value);
        cycles = 4;
        break;
    }
    case 0x8C: { // DEC !abs
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        const auto value = static_cast<std::uint8_t>(
            bus_.spc_read(address) - 1U);
        bus_.spc_write(address, value);
        set_nz8(value);
        cycles = 5;
        break;
    }
    case 0x9B: { // DEC dp+X, direct offset wraps
        const auto offset = static_cast<std::uint8_t>(fetch() + registers_.x);
        const auto value = static_cast<std::uint8_t>(read_direct(offset) - 1U);
        write_direct(offset, value);
        set_nz8(value);
        cycles = 5;
        break;
    }
    case 0xBB: { // INC dp+X, direct offset wraps
        const auto offset = static_cast<std::uint8_t>(fetch() + registers_.x);
        const auto value = static_cast<std::uint8_t>(read_direct(offset) + 1U);
        write_direct(offset, value);
        set_nz8(value);
        cycles = 5;
        break;
    }
    case 0x3A: { // INCW dp
        const auto offset = fetch();
        const auto low = read_direct(offset);
        const auto high = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto value = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + 1U);
        write_direct(offset, static_cast<std::uint8_t>(value));
        write_direct(static_cast<std::uint8_t>(offset + 1),
                     static_cast<std::uint8_t>(value >> 8));
        set_nz16(value);
        cycles = 6;
        break;
    }
    case 0xBA: {
        const auto offset = fetch();
        registers_.a = read_direct(offset);
        registers_.y = read_direct(static_cast<std::uint8_t>(offset + 1));
        set_nz16(static_cast<std::uint16_t>(
            registers_.a | (static_cast<unsigned>(registers_.y) << 8)));
        cycles = 5;
        break;
    }
    case 0xDA: {
        const auto offset = fetch();
        write_direct(offset, registers_.a);
        write_direct(static_cast<std::uint8_t>(offset + 1), registers_.y);
        cycles = 5;
        break;
    }
    case 0xC4: write_direct(fetch(), registers_.a); cycles = 4; break;
    case 0xC7: { // MOV [dp+X],A
        const auto offset = static_cast<std::uint8_t>(fetch() + registers_.x);
        const auto low = read_direct(offset);
        const auto high = read_direct(static_cast<std::uint8_t>(offset + 1));
        bus_.spc_write(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), registers_.a);
        cycles = 7;
        break;
    }
    case 0xD5: { // MOV !abs+X,A
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.x);
        bus_.spc_write(address, registers_.a);
        cycles = 6;
        break;
    }
    case 0xCC: { // MOV !abs,Y
        const auto low = fetch();
        const auto high = fetch();
        bus_.spc_write(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), registers_.y);
        cycles = 5;
        break;
    }
    case 0xC5: { // MOV !abs,A
        const auto low = fetch();
        const auto high = fetch();
        bus_.spc_write(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), registers_.a);
        cycles = 5;
        break;
    }
    case 0xDD: registers_.a = registers_.y; set_nz8(registers_.a); cycles = 2; break;
    case 0xFD: registers_.y = registers_.a; set_nz8(registers_.y); cycles = 2; break;
    case 0x5D: registers_.x = registers_.a; set_nz8(registers_.x); cycles = 2; break;
    case 0x1F: {
        const auto lo = fetch();
        const auto hi = fetch();
        const auto address = static_cast<std::uint16_t>(
            (lo | (static_cast<unsigned>(hi) << 8)) + registers_.x);
        const auto target_lo = bus_.spc_read(address);
        const auto target_hi = bus_.spc_read(static_cast<std::uint16_t>(address + 1));
        registers_.pc = static_cast<std::uint16_t>(
            target_lo | (static_cast<unsigned>(target_hi) << 8));
        cycles = 6;
        break;
    }
    default:
        if ((opcode & 0x1FU) == 0x02U) { // SET1 dp.bit
            const auto offset = fetch();
            write_direct(offset, static_cast<std::uint8_t>(
                read_direct(offset) | (1U << (opcode >> 5))));
            cycles = 4;
            break;
        }
        if ((opcode & 0x1FU) == 0x12U) { // CLR1 dp.bit
            const auto offset = fetch();
            write_direct(offset, static_cast<std::uint8_t>(
                read_direct(offset) & ~(1U << (opcode >> 5))));
            cycles = 4;
            break;
        }
        if ((opcode & 0x1FU) == 0x03U || (opcode & 0x1FU) == 0x13U) {
            const auto offset = fetch();
            const auto value = read_direct(offset);
            const auto displacement = static_cast<std::int8_t>(fetch());
            const auto bit = static_cast<std::uint8_t>(1U << (opcode >> 5));
            const bool bit_set = (value & bit) != 0;
            const bool take = (opcode & 0x10U) != 0 ? !bit_set : bit_set;
            if (take) {
                registers_.pc = static_cast<std::uint16_t>(
                    registers_.pc + displacement);
            }
            cycles = take ? 7U : 5U;
            break;
        }
        registers_.pc = start;
        return {0, opcode, false};
    }
    bus_.tick(cycles);
    return {cycles, opcode, true};
}

} // namespace gameboy
