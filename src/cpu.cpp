#include "gameboy/cpu.hpp"

#include "gameboy/memory_bus.hpp"

#include <iomanip>
#include <sstream>

namespace gameboy {

UnsupportedOpcode::UnsupportedOpcode(const std::uint8_t opcode,
                                     const std::uint16_t address)
    : std::runtime_error([=] {
          std::ostringstream message;
          message << "Unsupported opcode 0x" << std::hex << std::setw(2)
                  << std::setfill('0') << static_cast<unsigned>(opcode)
                  << " at 0x" << std::setw(4) << address;
          return message.str();
      }()) {}

Cpu::Cpu() { reset(); }

void Cpu::reset() noexcept {
    registers_ = {
        0x01, 0xB0, 0x00, 0x13, 0x00, 0xD8, 0x01, 0x4D, 0xFFFE, 0x0100,
    };
    ime_ = false;
    halted_ = false;
    stopped_ = false;
    halt_bug_ = false;
    ime_enable_delay_ = 0;
    step_cycles_ = 0;
    total_cycles_ = 0;
}

void Cpu::load_registers(CpuRegisters registers) noexcept {
    registers.f &= 0xF0;
    registers_ = registers;
}

unsigned Cpu::step(MemoryBus& bus) {
    step_cycles_ = 0;
    const auto pending = pending_interrupts(bus);
    if (halted_ || stopped_) {
        if (pending == 0) {
            if (halted_) idle(bus, 4);
            total_cycles_ += 4;
            return 4;
        }
        halted_ = false;
        stopped_ = false;
    }

    if (ime_ && pending != 0) {
        const auto cycles = service_interrupt(bus, pending);
        if (step_cycles_ < cycles) idle(bus, cycles - step_cycles_);
        total_cycles_ += cycles;
        return cycles;
    }

    const auto cycles = execute_instruction(bus);
    if (step_cycles_ > cycles) {
        throw std::logic_error("CPU bus accesses exceeded instruction timing");
    }
    if (step_cycles_ < cycles) idle(bus, cycles - step_cycles_);
    if (ime_enable_delay_ != 0 && --ime_enable_delay_ == 0) {
        ime_ = true;
    }
    total_cycles_ += cycles;
    return cycles;
}

unsigned Cpu::execute_instruction(MemoryBus& bus) {
    const auto opcode_address = registers_.pc;
    const auto opcode = fetch8(bus);

    if (opcode >= 0x40 && opcode <= 0x7F && opcode != 0x76) {
        const auto destination = static_cast<unsigned>((opcode >> 3) & 0x07);
        const auto source = static_cast<unsigned>(opcode & 0x07);
        write_register(destination, read_register(source, bus), bus);
        return destination == 6 || source == 6 ? 8 : 4;
    }

    if (opcode >= 0x80 && opcode <= 0xBF) {
        const auto operation = static_cast<unsigned>((opcode >> 3) & 0x07);
        const auto source = static_cast<unsigned>(opcode & 0x07);
        const auto value = read_register(source, bus);
        switch (operation) {
        case 0: add(value, false); break;
        case 1: add(value, true); break;
        case 2: subtract(value, false); break;
        case 3: subtract(value, true); break;
        case 4:
            registers_.a &= value;
            registers_.f = static_cast<std::uint8_t>(
                (registers_.a == 0 ? zero_flag : 0) | half_carry_flag);
            break;
        case 5:
            registers_.a ^= value;
            registers_.f = registers_.a == 0 ? zero_flag : 0;
            break;
        case 6:
            registers_.a |= value;
            registers_.f = registers_.a == 0 ? zero_flag : 0;
            break;
        case 7: compare(value); break;
        }
        return source == 6 ? 8 : 4;
    }

    if ((opcode & 0xC7) == 0x04) {
        const auto target = static_cast<unsigned>((opcode >> 3) & 0x07);
        write_register(target, increment(read_register(target, bus)), bus);
        return target == 6 ? 12 : 4;
    }
    if ((opcode & 0xC7) == 0x05) {
        const auto target = static_cast<unsigned>((opcode >> 3) & 0x07);
        write_register(target, decrement(read_register(target, bus)), bus);
        return target == 6 ? 12 : 4;
    }
    if ((opcode & 0xC7) == 0x06) {
        const auto target = static_cast<unsigned>((opcode >> 3) & 0x07);
        write_register(target, fetch8(bus), bus);
        return target == 6 ? 12 : 8;
    }

    if ((opcode & 0xCF) == 0x01) {
        set_register_pair((opcode >> 4) & 0x03, fetch16(bus));
        return 12;
    }
    if ((opcode & 0xCF) == 0x03) {
        const auto pair = static_cast<unsigned>((opcode >> 4) & 0x03);
        set_register_pair(pair,
                          static_cast<std::uint16_t>(register_pair(pair) + 1));
        return 8;
    }
    if ((opcode & 0xCF) == 0x0B) {
        const auto pair = static_cast<unsigned>((opcode >> 4) & 0x03);
        set_register_pair(pair,
                          static_cast<std::uint16_t>(register_pair(pair) - 1));
        return 8;
    }
    if ((opcode & 0xCF) == 0x09) {
        const auto value = register_pair((opcode >> 4) & 0x03);
        const auto old_hl = hl();
        const auto result = static_cast<std::uint32_t>(old_hl) + value;
        const auto preserved_zero = static_cast<std::uint8_t>(registers_.f & zero_flag);
        registers_.f = static_cast<std::uint8_t>(
            preserved_zero |
            (((old_hl & 0x0FFF) + (value & 0x0FFF) > 0x0FFF)
                 ? half_carry_flag
                 : 0) |
            (result > 0xFFFF ? carry_flag : 0));
        set_hl(static_cast<std::uint16_t>(result));
        return 8;
    }
    if ((opcode & 0xCF) == 0xC1) { // POP qq
        const auto pair = static_cast<unsigned>((opcode >> 4) & 0x03);
        set_stack_register_pair(pair, pop(bus));
        return 12;
    }
    if ((opcode & 0xCF) == 0xC5) { // PUSH qq
        const auto pair = static_cast<unsigned>((opcode >> 4) & 0x03);
        push(bus, stack_register_pair(pair));
        return 16;
    }
    if ((opcode & 0xE7) == 0x20) { // JR cc,e8
        const auto offset_byte = fetch8(bus);
        if (!condition((opcode >> 3) & 0x03)) {
            return 8;
        }
        const auto offset = offset_byte < 0x80
                                ? static_cast<int>(offset_byte)
                                : static_cast<int>(offset_byte) - 0x100;
        registers_.pc = static_cast<std::uint16_t>(registers_.pc + offset);
        return 12;
    }
    if ((opcode & 0xE7) == 0xC2) { // JP cc,a16
        const auto address = fetch16(bus);
        if (!condition((opcode >> 3) & 0x03)) {
            return 12;
        }
        registers_.pc = address;
        return 16;
    }
    if ((opcode & 0xE7) == 0xC4) { // CALL cc,a16
        const auto address = fetch16(bus);
        if (!condition((opcode >> 3) & 0x03)) {
            return 12;
        }
        push(bus, registers_.pc);
        registers_.pc = address;
        return 24;
    }
    if ((opcode & 0xE7) == 0xC0) { // RET cc
        if (!condition((opcode >> 3) & 0x03)) {
            return 8;
        }
        idle(bus, 4);
        registers_.pc = pop(bus);
        return 20;
    }
    if ((opcode & 0xC7) == 0xC7) { // RST vec
        push(bus, registers_.pc);
        registers_.pc = static_cast<std::uint16_t>(opcode & 0x38);
        return 16;
    }

    switch (opcode) {
    case 0x00: return 4; // NOP
    case 0x02:
        write8(bus, bc(), registers_.a);
        return 8;
    case 0x07: { // RLCA
        const auto carry = (registers_.a & 0x80) != 0;
        registers_.a = static_cast<std::uint8_t>(
            (registers_.a << 1) | (carry ? 1 : 0));
        registers_.f = carry ? carry_flag : 0;
        return 4;
    }
    case 0x08: {
        const auto address = fetch16(bus);
        write8(bus, address, static_cast<std::uint8_t>(registers_.sp));
        write8(bus, static_cast<std::uint16_t>(address + 1),
               static_cast<std::uint8_t>(registers_.sp >> 8));
        return 20;
    }
    case 0x0A:
        registers_.a = read8(bus, bc());
        return 8;
    case 0x0F: { // RRCA
        const auto carry = (registers_.a & 0x01) != 0;
        registers_.a = static_cast<std::uint8_t>(
            (registers_.a >> 1) | (carry ? 0x80 : 0));
        registers_.f = carry ? carry_flag : 0;
        return 4;
    }
    case 0x10: // STOP 0
        static_cast<void>(bus.read8(registers_.pc));
        ++registers_.pc;
        bus.write8(0xFF04, 0);
        stopped_ = true;
        return 4;
    case 0x12:
        write8(bus, de(), registers_.a);
        return 8;
    case 0x17: { // RLA
        const auto old_carry = flag(carry_flag);
        const auto new_carry = (registers_.a & 0x80) != 0;
        registers_.a = static_cast<std::uint8_t>(
            (registers_.a << 1) | (old_carry ? 1 : 0));
        registers_.f = new_carry ? carry_flag : 0;
        return 4;
    }
    case 0x18: { // JR e8
        const auto offset_byte = fetch8(bus);
        const auto offset = offset_byte < 0x80
                                ? static_cast<int>(offset_byte)
                                : static_cast<int>(offset_byte) - 0x100;
        registers_.pc = static_cast<std::uint16_t>(registers_.pc + offset);
        return 12;
    }
    case 0x1A:
        registers_.a = read8(bus, de());
        return 8;
    case 0x1F: { // RRA
        const auto old_carry = flag(carry_flag);
        const auto new_carry = (registers_.a & 0x01) != 0;
        registers_.a = static_cast<std::uint8_t>(
            (registers_.a >> 1) | (old_carry ? 0x80 : 0));
        registers_.f = new_carry ? carry_flag : 0;
        return 4;
    }
    case 0x22:
        write8(bus, hl(), registers_.a);
        set_hl(static_cast<std::uint16_t>(hl() + 1));
        return 8;
    case 0x27:
        decimal_adjust();
        return 4;
    case 0x2A:
        registers_.a = read8(bus, hl());
        set_hl(static_cast<std::uint16_t>(hl() + 1));
        return 8;
    case 0x2F:
        registers_.a = static_cast<std::uint8_t>(~registers_.a);
        registers_.f = static_cast<std::uint8_t>(
            registers_.f | subtract_flag | half_carry_flag);
        return 4;
    case 0x32:
        write8(bus, hl(), registers_.a);
        set_hl(static_cast<std::uint16_t>(hl() - 1));
        return 8;
    case 0x37:
        registers_.f = static_cast<std::uint8_t>(
            (registers_.f & zero_flag) | carry_flag);
        return 4;
    case 0x3A:
        registers_.a = read8(bus, hl());
        set_hl(static_cast<std::uint16_t>(hl() - 1));
        return 8;
    case 0x3F:
        registers_.f = static_cast<std::uint8_t>(
            (registers_.f & zero_flag) |
            (flag(carry_flag) ? 0 : carry_flag));
        return 4;
    case 0x76: // HALT
        if (!ime_ && pending_interrupts(bus) != 0) {
            halt_bug_ = true;
        } else {
            halted_ = true;
        }
        return 4;
    case 0xC3:
        registers_.pc = fetch16(bus);
        return 16;
    case 0xC9:
        registers_.pc = pop(bus);
        return 16;
    case 0xCB:
        return execute_cb(bus);
    case 0xCD: {
        const auto address = fetch16(bus);
        push(bus, registers_.pc);
        registers_.pc = address;
        return 24;
    }
    case 0xC6:
        add(fetch8(bus), false);
        return 8;
    case 0xCE:
        add(fetch8(bus), true);
        return 8;
    case 0xD6:
        subtract(fetch8(bus), false);
        return 8;
    case 0xD9:
        registers_.pc = pop(bus);
        ime_ = true;
        ime_enable_delay_ = 0;
        return 16;
    case 0xDE:
        subtract(fetch8(bus), true);
        return 8;
    case 0xE0:
        write8(bus, static_cast<std::uint16_t>(0xFF00 | fetch8(bus)), registers_.a);
        return 12;
    case 0xE2:
        write8(bus, static_cast<std::uint16_t>(0xFF00 | registers_.c), registers_.a);
        return 8;
    case 0xE6:
        registers_.a &= fetch8(bus);
        registers_.f = static_cast<std::uint8_t>(
            (registers_.a == 0 ? zero_flag : 0) | half_carry_flag);
        return 8;
    case 0xE8: {
        const auto offset_byte = fetch8(bus);
        const auto offset = offset_byte < 0x80
                                ? static_cast<int>(offset_byte)
                                : static_cast<int>(offset_byte) - 0x100;
        const auto old_sp = registers_.sp;
        registers_.f = static_cast<std::uint8_t>(
            (((old_sp & 0x0F) + (offset_byte & 0x0F) > 0x0F)
                 ? half_carry_flag
                 : 0) |
            (((old_sp & 0xFF) + offset_byte > 0xFF) ? carry_flag : 0));
        registers_.sp = static_cast<std::uint16_t>(old_sp + offset);
        return 16;
    }
    case 0xE9:
        registers_.pc = hl();
        return 4;
    case 0xEA: {
        const auto address = fetch16(bus);
        write8(bus, address, registers_.a);
        return 16;
    }
    case 0xEE:
        registers_.a ^= fetch8(bus);
        registers_.f = registers_.a == 0 ? zero_flag : 0;
        return 8;
    case 0xF0:
        registers_.a = read8(bus,
            static_cast<std::uint16_t>(0xFF00 | fetch8(bus)));
        return 12;
    case 0xF2:
        registers_.a = read8(bus,
            static_cast<std::uint16_t>(0xFF00 | registers_.c));
        return 8;
    case 0xF3:
        ime_ = false;
        ime_enable_delay_ = 0;
        return 4;
    case 0xF6:
        registers_.a |= fetch8(bus);
        registers_.f = registers_.a == 0 ? zero_flag : 0;
        return 8;
    case 0xF8: {
        const auto offset_byte = fetch8(bus);
        const auto offset = offset_byte < 0x80
                                ? static_cast<int>(offset_byte)
                                : static_cast<int>(offset_byte) - 0x100;
        const auto old_sp = registers_.sp;
        registers_.f = static_cast<std::uint8_t>(
            (((old_sp & 0x0F) + (offset_byte & 0x0F) > 0x0F)
                 ? half_carry_flag
                 : 0) |
            (((old_sp & 0xFF) + offset_byte > 0xFF) ? carry_flag : 0));
        set_hl(static_cast<std::uint16_t>(old_sp + offset));
        return 12;
    }
    case 0xF9:
        registers_.sp = hl();
        return 8;
    case 0xFA:
        registers_.a = read8(bus, fetch16(bus));
        return 16;
    case 0xFB:
        ime_enable_delay_ = 2;
        return 4;
    case 0xFE:
        compare(fetch8(bus));
        return 8;
    default:
        throw UnsupportedOpcode(opcode, opcode_address);
    }
}

const CpuRegisters& Cpu::registers() const noexcept { return registers_; }

bool Cpu::halted() const noexcept { return halted_; }

bool Cpu::stopped() const noexcept { return stopped_; }

bool Cpu::interrupts_enabled() const noexcept { return ime_; }

std::uint64_t Cpu::total_cycles() const noexcept { return total_cycles_; }

unsigned Cpu::execute_cb(MemoryBus& bus) {
    const auto opcode = fetch8(bus);
    const auto group = static_cast<unsigned>(opcode >> 6);
    const auto operation = static_cast<unsigned>((opcode >> 3) & 0x07);
    const auto target = static_cast<unsigned>(opcode & 0x07);
    const auto value = read_register(target, bus);

    if (group == 0) {
        std::uint8_t result = 0;
        bool carry = false;
        switch (operation) {
        case 0: // RLC
            carry = (value & 0x80) != 0;
            result = static_cast<std::uint8_t>(
                (value << 1) | (carry ? 1 : 0));
            break;
        case 1: // RRC
            carry = (value & 0x01) != 0;
            result = static_cast<std::uint8_t>(
                (value >> 1) | (carry ? 0x80 : 0));
            break;
        case 2: { // RL
            const auto old_carry = flag(carry_flag);
            carry = (value & 0x80) != 0;
            result = static_cast<std::uint8_t>(
                (value << 1) | (old_carry ? 1 : 0));
            break;
        }
        case 3: { // RR
            const auto old_carry = flag(carry_flag);
            carry = (value & 0x01) != 0;
            result = static_cast<std::uint8_t>(
                (value >> 1) | (old_carry ? 0x80 : 0));
            break;
        }
        case 4: // SLA
            carry = (value & 0x80) != 0;
            result = static_cast<std::uint8_t>(value << 1);
            break;
        case 5: // SRA
            carry = (value & 0x01) != 0;
            result = static_cast<std::uint8_t>(
                (value >> 1) | (value & 0x80));
            break;
        case 6: // SWAP
            result = static_cast<std::uint8_t>((value << 4) | (value >> 4));
            break;
        case 7: // SRL
            carry = (value & 0x01) != 0;
            result = static_cast<std::uint8_t>(value >> 1);
            break;
        }
        write_register(target, result, bus);
        registers_.f = static_cast<std::uint8_t>(
            (result == 0 ? zero_flag : 0) | (carry ? carry_flag : 0));
        return target == 6 ? 16 : 8;
    }

    const auto mask = static_cast<std::uint8_t>(1U << operation);
    if (group == 1) { // BIT b,r
        registers_.f = static_cast<std::uint8_t>(
            (registers_.f & carry_flag) | half_carry_flag |
            ((value & mask) == 0 ? zero_flag : 0));
        return target == 6 ? 12 : 8;
    }

    const auto result = group == 2
                            ? static_cast<std::uint8_t>(value & ~mask)
                            : static_cast<std::uint8_t>(value | mask);
    write_register(target, result, bus);
    return target == 6 ? 16 : 8;
}

std::uint8_t Cpu::pending_interrupts(const MemoryBus& bus) const noexcept {
    return static_cast<std::uint8_t>(bus.read8(0xFFFF) & bus.read8(0xFF0F) & 0x1F);
}

unsigned Cpu::service_interrupt(MemoryBus& bus,
                                const std::uint8_t pending) noexcept {
    unsigned interrupt = 0;
    while ((pending & (1U << interrupt)) == 0) {
        ++interrupt;
    }
    const auto mask = static_cast<std::uint8_t>(1U << interrupt);
    bus.write8(0xFF0F, static_cast<std::uint8_t>(bus.read8(0xFF0F) & ~mask));
    ime_ = false;
    ime_enable_delay_ = 0;
    halted_ = false;
    stopped_ = false;
    halt_bug_ = false;
    idle(bus, 8);
    --registers_.sp;
    write8(bus, registers_.sp, static_cast<std::uint8_t>(registers_.pc >> 8));
    --registers_.sp;
    write8(bus, registers_.sp, static_cast<std::uint8_t>(registers_.pc));
    idle(bus, 4);
    registers_.pc = static_cast<std::uint16_t>(0x0040 + interrupt * 8);
    return 20;
}

bool Cpu::condition(const unsigned index) const noexcept {
    switch (index) {
    case 0: return !flag(zero_flag);
    case 1: return flag(zero_flag);
    case 2: return !flag(carry_flag);
    default: return flag(carry_flag);
    }
}

void Cpu::idle(MemoryBus& bus, const unsigned cycles) noexcept {
    bus.tick(cycles);
    step_cycles_ += cycles;
}

std::uint8_t Cpu::read8(MemoryBus& bus, const std::uint16_t address) noexcept {
    idle(bus, 4);
    return bus.read8(address);
}

void Cpu::write8(MemoryBus& bus, const std::uint16_t address,
                 const std::uint8_t value) noexcept {
    idle(bus, 4);
    bus.write8(address, value);
}

void Cpu::push(MemoryBus& bus, const std::uint16_t value) noexcept {
    idle(bus, 4);
    --registers_.sp;
    write8(bus, registers_.sp, static_cast<std::uint8_t>(value >> 8));
    --registers_.sp;
    write8(bus, registers_.sp, static_cast<std::uint8_t>(value));
}

std::uint16_t Cpu::pop(MemoryBus& bus) noexcept {
    const auto low = read8(bus, registers_.sp++);
    const auto high = read8(bus, registers_.sp++);
    return static_cast<std::uint16_t>(
        low | (static_cast<std::uint16_t>(high) << 8));
}

std::uint8_t Cpu::fetch8(MemoryBus& bus) noexcept {
    const auto value = read8(bus, registers_.pc);
    if (halt_bug_) {
        halt_bug_ = false;
    } else {
        ++registers_.pc;
    }
    return value;
}

std::uint16_t Cpu::fetch16(MemoryBus& bus) noexcept {
    const auto low = fetch8(bus);
    const auto high = fetch8(bus);
    return static_cast<std::uint16_t>(low | (static_cast<std::uint16_t>(high) << 8));
}

std::uint8_t Cpu::read_register(const unsigned index, MemoryBus& bus) noexcept {
    switch (index) {
    case 0: return registers_.b;
    case 1: return registers_.c;
    case 2: return registers_.d;
    case 3: return registers_.e;
    case 4: return registers_.h;
    case 5: return registers_.l;
    case 6: return read8(bus, hl());
    default: return registers_.a;
    }
}

void Cpu::write_register(const unsigned index, const std::uint8_t value,
                         MemoryBus& bus) noexcept {
    switch (index) {
    case 0: registers_.b = value; break;
    case 1: registers_.c = value; break;
    case 2: registers_.d = value; break;
    case 3: registers_.e = value; break;
    case 4: registers_.h = value; break;
    case 5: registers_.l = value; break;
    case 6: write8(bus, hl(), value); break;
    default: registers_.a = value; break;
    }
}

std::uint16_t Cpu::bc() const noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(registers_.b) << 8) |
                                      registers_.c);
}

std::uint16_t Cpu::de() const noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(registers_.d) << 8) |
                                      registers_.e);
}

std::uint16_t Cpu::hl() const noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(registers_.h) << 8) |
                                      registers_.l);
}

std::uint16_t Cpu::register_pair(const unsigned index) const noexcept {
    switch (index) {
    case 0: return bc();
    case 1: return de();
    case 2: return hl();
    default: return registers_.sp;
    }
}

std::uint16_t Cpu::stack_register_pair(const unsigned index) const noexcept {
    if (index == 3) {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(registers_.a) << 8) | registers_.f);
    }
    return register_pair(index);
}

void Cpu::set_bc(const std::uint16_t value) noexcept {
    registers_.b = static_cast<std::uint8_t>(value >> 8);
    registers_.c = static_cast<std::uint8_t>(value);
}

void Cpu::set_de(const std::uint16_t value) noexcept {
    registers_.d = static_cast<std::uint8_t>(value >> 8);
    registers_.e = static_cast<std::uint8_t>(value);
}

void Cpu::set_hl(const std::uint16_t value) noexcept {
    registers_.h = static_cast<std::uint8_t>(value >> 8);
    registers_.l = static_cast<std::uint8_t>(value);
}

void Cpu::set_register_pair(const unsigned index, const std::uint16_t value) noexcept {
    switch (index) {
    case 0: set_bc(value); break;
    case 1: set_de(value); break;
    case 2: set_hl(value); break;
    default: registers_.sp = value; break;
    }
}

void Cpu::set_stack_register_pair(const unsigned index,
                                  const std::uint16_t value) noexcept {
    if (index == 3) {
        registers_.a = static_cast<std::uint8_t>(value >> 8);
        registers_.f = static_cast<std::uint8_t>(value & 0xF0);
        return;
    }
    set_register_pair(index, value);
}

bool Cpu::flag(const std::uint8_t mask) const noexcept {
    return (registers_.f & mask) != 0;
}

void Cpu::add(const std::uint8_t value, const bool with_carry) noexcept {
    const auto carry = static_cast<std::uint8_t>(with_carry && flag(carry_flag));
    const auto old_a = registers_.a;
    const auto result = static_cast<std::uint16_t>(old_a) + value + carry;
    registers_.a = static_cast<std::uint8_t>(result);
    registers_.f = static_cast<std::uint8_t>(
        (registers_.a == 0 ? zero_flag : 0) |
        (((old_a & 0x0F) + (value & 0x0F) + carry > 0x0F)
             ? half_carry_flag
             : 0) |
        (result > 0xFF ? carry_flag : 0));
}

void Cpu::subtract(const std::uint8_t value, const bool with_carry) noexcept {
    const auto carry = static_cast<std::uint8_t>(with_carry && flag(carry_flag));
    const auto old_a = registers_.a;
    const auto subtrahend = static_cast<std::uint16_t>(value) + carry;
    registers_.a = static_cast<std::uint8_t>(old_a - subtrahend);
    registers_.f = static_cast<std::uint8_t>(
        (registers_.a == 0 ? zero_flag : 0) | subtract_flag |
        ((old_a & 0x0F) < ((value & 0x0F) + carry) ? half_carry_flag : 0) |
        (static_cast<std::uint16_t>(old_a) < subtrahend ? carry_flag : 0));
}

void Cpu::compare(const std::uint8_t value) noexcept {
    const auto old_a = registers_.a;
    subtract(value, false);
    registers_.a = old_a;
}

std::uint8_t Cpu::increment(const std::uint8_t value) noexcept {
    const auto result = static_cast<std::uint8_t>(value + 1);
    registers_.f = static_cast<std::uint8_t>(
        (registers_.f & carry_flag) |
        (result == 0 ? zero_flag : 0) |
        ((value & 0x0F) == 0x0F ? half_carry_flag : 0));
    return result;
}

std::uint8_t Cpu::decrement(const std::uint8_t value) noexcept {
    const auto result = static_cast<std::uint8_t>(value - 1);
    registers_.f = static_cast<std::uint8_t>(
        (registers_.f & carry_flag) |
        (result == 0 ? zero_flag : 0) | subtract_flag |
        ((value & 0x0F) == 0 ? half_carry_flag : 0));
    return result;
}

void Cpu::decimal_adjust() noexcept {
    std::uint8_t correction = 0;
    auto carry = flag(carry_flag);
    if (!flag(subtract_flag)) {
        if (carry || registers_.a > 0x99) {
            correction |= 0x60;
            carry = true;
        }
        if (flag(half_carry_flag) || (registers_.a & 0x0F) > 0x09) {
            correction |= 0x06;
        }
        registers_.a = static_cast<std::uint8_t>(registers_.a + correction);
    } else {
        if (carry) correction |= 0x60;
        if (flag(half_carry_flag)) correction |= 0x06;
        registers_.a = static_cast<std::uint8_t>(registers_.a - correction);
    }
    registers_.f = static_cast<std::uint8_t>(
        (registers_.f & subtract_flag) |
        (registers_.a == 0 ? zero_flag : 0) |
        (carry ? carry_flag : 0));
}

} // namespace gameboy
