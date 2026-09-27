#include "snes_65c816_trace_cpu.hpp"

namespace sgb_test {
namespace {
constexpr std::uint8_t carry = 0x01;
constexpr std::uint8_t zero = 0x02;
constexpr std::uint8_t irq_disable = 0x04;
constexpr std::uint8_t decimal = 0x08;
constexpr std::uint8_t index_width = 0x10;
constexpr std::uint8_t memory_width = 0x20;
constexpr std::uint8_t negative = 0x80;
} // namespace

Snes65c816TraceCpu::Snes65c816TraceCpu(
    const gameboy::SgbProgramRom& rom, gameboy::SnesApuBus& apu) noexcept
    : rom_(rom), apu_(apu) {
    r_.pc = rom.reset_vector();
}

std::uint8_t Snes65c816TraceCpu::read8(const std::uint8_t bank,
                                        const std::uint16_t address) noexcept {
    if (bank == 0x7E || bank == 0x7F) {
        return wram_[(static_cast<unsigned>(bank - 0x7E) << 16) | address];
    }
    const bool system_bank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
    if (system_bank && address < 0x2000) return wram_[address];
    if (system_bank && address >= 0x2140 && address <= 0x2143) {
        return apu_.host_read_port(address - 0x2140);
    }
    if (system_bank && address >= 0x6000 && address <= 0x7FFF) {
        // ICD packet/row/status reads need an explicit Game Boy-side source.
        error_ = Error::unsupported_read;
        error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
        return 0;
    }
    if (system_bank && address >= 0x2000 && address < 0x6000) {
        error_ = Error::unsupported_read;
        error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
        return 0;
    }
    if (address >= 0x8000) return rom_.read(bank, address);
    error_ = Error::unsupported_read;
    error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
    return 0;
}

void Snes65c816TraceCpu::write8(const std::uint8_t bank,
                                const std::uint16_t address,
                                const std::uint8_t value) noexcept {
    if (bank == 0x7E || bank == 0x7F) {
        wram_[(static_cast<unsigned>(bank - 0x7E) << 16) | address] = value;
        return;
    }
    const bool system_bank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
    if (system_bank && address < 0x2000) {
        wram_[address] = value;
        return;
    }
    if (system_bank && address >= 0x2140 && address <= 0x2143) {
        if (apu_write_count_ == apu_writes_.size()) {
            error_ = Error::trace_overflow;
            error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
            return;
        }
        apu_.host_write_port(address - 0x2140, value);
        apu_writes_[apu_write_count_++] = {steps_ + 1,
            static_cast<std::uint8_t>(address - 0x2140), value};
        return;
    }
    if (system_bank && (address == 0x6001 || address == 0x6003 ||
                        (address >= 0x6004 && address <= 0x6007))) {
        // Write-only ICD control and joypad forwarding. This trace runs only
        // until the first APU write; GB-side effects are not synthesized.
        return;
    }
    // PPU/DMA/CPU registers are write-only in this bounded trace. We permit
    // initialization writes but never fabricate readback or side effects.
    if (system_bank && address >= 0x2000 && address < 0x6000) return;
    error_ = Error::unsupported_write;
    error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
}

std::uint8_t Snes65c816TraceCpu::fetch8() noexcept {
    const auto value = read8(r_.pb, r_.pc);
    ++r_.pc;
    return value;
}

std::uint16_t Snes65c816TraceCpu::fetch16() noexcept {
    const auto low = fetch8();
    return static_cast<std::uint16_t>(low | (static_cast<unsigned>(fetch8()) << 8));
}

std::uint16_t Snes65c816TraceCpu::read16(const std::uint8_t bank,
                                          const std::uint16_t address) noexcept {
    const auto low = read8(bank, address);
    return static_cast<std::uint16_t>(low | (static_cast<unsigned>(read8(
        bank, static_cast<std::uint16_t>(address + 1))) << 8));
}

void Snes65c816TraceCpu::write16(const std::uint8_t bank,
                                  const std::uint16_t address,
                                  const std::uint16_t value) noexcept {
    write8(bank, address, static_cast<std::uint8_t>(value));
    write8(bank, static_cast<std::uint16_t>(address + 1),
           static_cast<std::uint8_t>(value >> 8));
}

void Snes65c816TraceCpu::push8(const std::uint8_t value) noexcept {
    write8(0, r_.s, value);
    r_.s = r_.e ? static_cast<std::uint16_t>(0x0100 | ((r_.s - 1) & 0xFF))
                : static_cast<std::uint16_t>(r_.s - 1);
}

std::uint8_t Snes65c816TraceCpu::pop8() noexcept {
    r_.s = r_.e ? static_cast<std::uint16_t>(0x0100 | ((r_.s + 1) & 0xFF))
                : static_cast<std::uint16_t>(r_.s + 1);
    return read8(0, r_.s);
}

void Snes65c816TraceCpu::set_nz8(const std::uint8_t value) noexcept {
    r_.p = static_cast<std::uint8_t>((r_.p & ~(negative | zero)) |
        (value & negative) | (value == 0 ? zero : 0));
}

void Snes65c816TraceCpu::set_nz16(const std::uint16_t value) noexcept {
    r_.p = static_cast<std::uint8_t>((r_.p & ~(negative | zero)) |
        ((value >> 8) & negative) | (value == 0 ? zero : 0));
}

void Snes65c816TraceCpu::set_index_width() noexcept {
    if (r_.e) r_.p = static_cast<std::uint8_t>(r_.p | memory_width | index_width);
    if (index_8()) {
        r_.x &= 0x00FF;
        r_.y &= 0x00FF;
    }
}

bool Snes65c816TraceCpu::accumulator_8() const noexcept {
    return r_.e || (r_.p & memory_width) != 0;
}

bool Snes65c816TraceCpu::index_8() const noexcept {
    return r_.e || (r_.p & index_width) != 0;
}

void Snes65c816TraceCpu::branch(const bool take) noexcept {
    const auto displacement = static_cast<std::int8_t>(fetch8());
    if (take) r_.pc = static_cast<std::uint16_t>(r_.pc + displacement);
}

Snes65c816TraceCpu::StepResult Snes65c816TraceCpu::step() noexcept {
    if (error_ != Error::none) return {error_, 0, r_.pb, r_.pc, error_address_};
    const auto bank = r_.pb;
    const auto pc = r_.pc;
    const auto opcode = fetch8();
    switch (opcode) {
    case 0x18: r_.p &= static_cast<std::uint8_t>(~carry); break; // CLC
    case 0x78: r_.p |= irq_disable; break; // SEI
    case 0xD8: r_.p &= static_cast<std::uint8_t>(~decimal); break; // CLD
    case 0xFB: { // XCE
        const bool old_carry = (r_.p & carry) != 0;
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) | (r_.e ? carry : 0));
        r_.e = old_carry;
        if (r_.e) r_.s = static_cast<std::uint16_t>(0x0100 | (r_.s & 0xFF));
        set_index_width();
        break;
    }
    case 0xE2: r_.p |= fetch8(); set_index_width(); break; // SEP
    case 0xC2: r_.p &= static_cast<std::uint8_t>(~fetch8());
               set_index_width(); break; // REP
    case 0xA9: { // LDA #imm
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00) | fetch8());
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = fetch16();
            set_nz16(r_.a);
        }
        break;
    }
    case 0xA2: { // LDX #imm
        r_.x = index_8() ? fetch8() : fetch16();
        if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
        else set_nz16(r_.x);
        break;
    }
    case 0x8D: { // STA abs
        const auto address = fetch16();
        if (accumulator_8()) write8(r_.db, address, static_cast<std::uint8_t>(r_.a));
        else write16(r_.db, address, r_.a);
        break;
    }
    case 0x8F: { // STA long
        const auto address = fetch16();
        const auto bank_byte = fetch8();
        if (accumulator_8()) write8(bank_byte, address, static_cast<std::uint8_t>(r_.a));
        else write16(bank_byte, address, r_.a);
        break;
    }
    case 0x9D: { // STA abs,X
        const auto address = static_cast<std::uint16_t>(fetch16() + r_.x);
        if (accumulator_8()) write8(r_.db, address, static_cast<std::uint8_t>(r_.a));
        else write16(r_.db, address, r_.a);
        break;
    }
    case 0x85: { // STA dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) write8(0, address, static_cast<std::uint8_t>(r_.a));
        else write16(0, address, r_.a);
        break;
    }
    case 0x9C: { // STZ abs
        const auto address = fetch16();
        if (accumulator_8()) write8(r_.db, address, 0);
        else write16(r_.db, address, 0);
        break;
    }
    case 0x74: { // STZ dp,X
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8() + r_.x);
        if (accumulator_8()) write8(0, address, 0);
        else write16(0, address, 0);
        break;
    }
    case 0xAD: { // LDA abs
        const auto address = fetch16();
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00) |
                read8(r_.db, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = read16(r_.db, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0x48: { // PHA
        if (!accumulator_8()) push8(static_cast<std::uint8_t>(r_.a >> 8));
        push8(static_cast<std::uint8_t>(r_.a));
        break;
    }
    case 0xAB: r_.db = pop8(); set_nz8(r_.db); break; // PLB
    case 0x2B: { // PLD
        const auto low = pop8();
        r_.d = static_cast<std::uint16_t>(low | (static_cast<unsigned>(pop8()) << 8));
        set_nz16(r_.d);
        break;
    }
    case 0x9A: r_.s = r_.e ? static_cast<std::uint16_t>(0x0100 | (r_.x & 0xFF))
                         : r_.x; break; // TXS
    case 0xE8: r_.x = static_cast<std::uint16_t>(
                   index_8() ? (r_.x + 1) & 0xFF : r_.x + 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x); break; // INX
    case 0xE0: { // CPX #imm
        const auto value = index_8() ? fetch8() : fetch16();
        const auto difference = static_cast<std::uint16_t>(r_.x - value);
        if (index_8()) set_nz8(static_cast<std::uint8_t>(difference));
        else set_nz16(difference);
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
            (r_.x >= value ? carry : 0));
        break;
    }
    case 0xD0: branch((r_.p & zero) == 0); break; // BNE
    case 0xF0: branch((r_.p & zero) != 0); break; // BEQ
    case 0x80: branch(true); break; // BRA
    case 0x4C: r_.pc = fetch16(); break; // JMP abs
    case 0x5C: { // JML long
        const auto target = fetch16();
        r_.pb = fetch8();
        r_.pc = target;
        break;
    }
    case 0x20: { // JSR abs
        const auto target = fetch16();
        const auto return_address = static_cast<std::uint16_t>(r_.pc - 1);
        push8(static_cast<std::uint8_t>(return_address >> 8));
        push8(static_cast<std::uint8_t>(return_address));
        r_.pc = target;
        break;
    }
    case 0x60: { // RTS
        const auto low = pop8();
        r_.pc = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(pop8()) << 8)) + 1);
        break;
    }
    default:
        r_.pc = pc;
        return {Error::unsupported_opcode, opcode, bank, pc,
                (static_cast<std::uint32_t>(bank) << 16) | pc};
    }
    if (error_ != Error::none) return {error_, opcode, bank, pc, error_address_};
    ++steps_;
    return {Error::none, opcode, bank, pc, 0};
}

} // namespace sgb_test
