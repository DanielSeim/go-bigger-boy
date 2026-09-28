#include "snes_65c816_trace_cpu.hpp"

#include "gameboy/snes_spc700.hpp"

#include <algorithm>

namespace sgb_test {
namespace {
constexpr std::uint8_t carry = 0x01;
constexpr std::uint8_t zero = 0x02;
constexpr std::uint8_t irq_disable = 0x04;
constexpr std::uint8_t decimal = 0x08;
constexpr std::uint8_t index_width = 0x10;
constexpr std::uint8_t memory_width = 0x20;
constexpr std::uint8_t negative = 0x80;
constexpr std::uint8_t overflow = 0x40;
} // namespace

unsigned SnesTraceTiming::line_length() const noexcept {
    return field_ && line_ == 240 ? 1360U : 1364U;
}

void SnesTraceTiming::advance(std::uint64_t master_clocks) noexcept {
    while (master_clocks != 0) {
        const auto span = static_cast<unsigned>(
            std::min<std::uint64_t>(master_clocks, line_length() - horizontal_clock_));
        horizontal_clock_ += span;
        clocks_ += span;
        master_clocks -= span;
        if (horizontal_clock_ == line_length()) {
            horizontal_clock_ = 0;
            refresh_done_ = false;
            if (++line_ == 262) {
                line_ = 0;
                field_ = !field_;
                nmi_latched_ = false;
                ++frames_;
                frame_start_clocks_ = clocks_;
            }
            if (line_ == (overscan_ ? 240U : 225U)) nmi_latched_ = true;
        }
    }
}

void SnesTraceTiming::cpu_cycle(unsigned master_clocks) noexcept {
    // The CPU loses a 40-master-clock WRAM refresh window near H=538.
    // The precise phase drifts a few clocks; this bounded trace pins the
    // first-line position, and does not claim sub-dot hardware parity.
    if (!refresh_done_ && horizontal_clock_ <= 538 &&
        horizontal_clock_ + master_clocks > 538) {
        const auto before = 538U - horizontal_clock_;
        advance(before);
        refresh_done_ = true;
        advance(40);
        master_clocks -= before;
    }
    advance(master_clocks);
}

std::uint8_t SnesTraceTiming::hvbjoy() const noexcept {
    const auto vblank_start = overscan_ ? 240U : 225U;
    const auto auto_start = autojoy_start();
    const bool busy = autojoy_enabled_ && autojoy_known() &&
        clocks_ >= auto_start && clocks_ < auto_start + 4224;
    return static_cast<std::uint8_t>((line_ >= vblank_start ? 0x80 : 0) |
        (horizontal_clock_ >= 1096 || horizontal_clock_ < 4 ? 0x40 : 0) |
        (busy ? 1 : 0));
}

bool SnesTraceTiming::autojoy_known() const noexcept {
    if (!autojoy_enabled_ || frames_ == 0) return true;
    const auto latest = frame_start_clocks_ +
        static_cast<std::uint64_t>(overscan_ ? 240U : 225U) * 1364 + 382;
    return autojoy_start() <= latest;
}

std::uint64_t SnesTraceTiming::autojoy_start() const noexcept {
    const auto vblank_start = overscan_ ? 240U : 225U;
    const auto first = static_cast<std::uint64_t>(vblank_start) * 1364 + 298;
    if (frames_ == 0) return first;
    // After the first-frame H=74.5-dot start, the controller clock chooses
    // the unique 256-master-clock phase within H=32.5..95.5 dots.
    const auto earliest = frame_start_clocks_ +
        static_cast<std::uint64_t>(vblank_start) * 1364 + 130;
    return earliest + (first + 256 - (earliest % 256)) % 256;
}

std::uint8_t SnesTraceTiming::stat78() noexcept {
    const auto result = static_cast<std::uint8_t>(
        0x02 | (field_ ? 0x80 : 0) | (latched_ ? 0x40 : 0));
    if (latch_enable_) latched_ = false;
    return result;
}

std::uint8_t SnesTraceTiming::rdnmi() noexcept {
    const auto result = static_cast<std::uint8_t>(0x02 | (nmi_latched_ ? 0x80 : 0));
    nmi_latched_ = false;
    return result;
}

void SnesTraceTiming::write_latch(const std::uint8_t value) noexcept {
    const bool enabled = (value & 0x80) != 0;
    if (latch_enable_ && !enabled) latched_ = true;
    latch_enable_ = enabled;
}

Snes65c816TraceCpu::Snes65c816TraceCpu(
    const gameboy::SgbProgramRom& rom, gameboy::SnesApuBus& apu,
    gameboy::SnesSpc700* spc) noexcept
    : rom_(rom), apu_(apu), spc_(spc) {
    r_.pc = rom.reset_vector();
}

void Snes65c816TraceCpu::synchronize_apu() noexcept {
    if (spc_ == nullptr || error_ != Error::none) return;
    // NTSC master oscillator (1.89e9/88 Hz) versus the 1.024 MHz S-SMP.
    // Instruction-granular rendezvous is deliberately bounded; sub-cycle
    // APU port ordering remains a separate validation task.
    constexpr std::uint64_t master_hz = 21'477'273;
    constexpr std::uint64_t spc_hz = 1'024'000;
    const auto target = timing_.clocks() * spc_hz / master_hz;
    while (spc_cycles_ < target) {
        const auto pc = spc_->registers().pc;
        const auto result = spc_->step();
        if (!result.supported) {
            error_ = Error::unsupported_spc_opcode;
            error_address_ = pc;
            return;
        }
        spc_cycles_ += result.cycles;
        if (spc_step_observer_ != nullptr)
            spc_step_observer_(spc_step_context_, spc_cycles_, result.opcode,
                               result.cycles);
    }
}

unsigned Snes65c816TraceCpu::bus_clocks(const std::uint8_t bank,
                                         const std::uint16_t address) const noexcept {
    if (bank == 0x7E || bank == 0x7F) return 8;
    const bool system_bank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
    if (system_bank) {
        if (address >= 0x4000 && address <= 0x41FF) return 12;
        if (address >= 0x2000 && address < 0x6000) return 6;
        if (address < 0x8000) return 8;
        return bank >= 0x80 && fast_rom_ ? 6U : 8U;
    }
    if (bank >= 0xC0 && fast_rom_) return 6;
    return 8;
}

unsigned Snes65c816TraceCpu::instruction_cycles(
    const std::uint8_t opcode, const bool memory8, const bool index8,
    const bool branch_taken, const bool branch_crossed) const noexcept {
    const unsigned m = memory8 ? 0U : 1U;
    const unsigned x = index8 ? 0U : 1U;
    const unsigned dp = (r_.d & 0xFFU) == 0 ? 0U : 1U;
    switch (opcode) {
    case 0x18: case 0x78: case 0xD8: case 0xFB:
    case 0x9A: case 0xE8: case 0xC8: case 0xCA: case 0x88:
    case 0x0A: case 0x3A: case 0x4A: case 0x98: case 0xA8:
    case 0xAA: case 0x8A: return 2;
    case 0xC2: case 0xE2: return 3;
    case 0xEB: return 3;
    case 0x09: case 0x29: case 0x49: case 0x69: case 0xC9: case 0xA9:
        return 2 + m;
    case 0xA0: case 0xA2: case 0xE0: return 2 + x;
    case 0xAC: return 4 + x;
    case 0x8C: case 0x8E: case 0xAE: return 4 + x;
    case 0x0D: case 0x2D: case 0x8D: case 0x9C: case 0xAD: case 0xCD:
        return 4 + m;
    case 0x19: case 0xB9: case 0xBD:
        return 4 + m + (indexed_extra_ ? 1U : 0U);
    case 0xB1: return 5 + m + dp + (indexed_extra_ ? 1U : 0U);
    case 0x97: case 0xB7: return 6 + m + dp;
    case 0xA7: return 6 + m + dp;
    case 0x5F: case 0x7F: case 0x8F: case 0x9F: case 0xAF: case 0xBF:
        return 5 + m;
    case 0x2E: return 6 + 2 * m;
    case 0x26: return 5 + 2 * m + dp;
    case 0x9D: case 0x9E: return 5 + m;
    case 0x64: case 0x65: case 0x85: case 0xA5: case 0xC5:
        return 3 + m + dp;
    case 0xA4: return 3 + x + dp;
    case 0xC6: case 0xE6: return 5 + 2 * m + dp;
    case 0xEE: return 6 + 2 * m;
    case 0x74: return 4 + m + dp;
    case 0x48: return 3 + m;
    case 0x08: case 0x8B: return 3;
    case 0x68: return 4 + m;
    case 0x28: return 4;
    case 0xAB: return 4;
    case 0x2B: return 5;
    case 0x10: case 0x30: case 0x90: case 0xD0: case 0xF0:
        return 2 + (branch_taken ? 1U : 0U) +
            (r_.e && branch_crossed ? 1U : 0U);
    case 0x80: return 3 + (r_.e && branch_crossed ? 1U : 0U);
    case 0x4C: return 3;
    case 0x5C: return 4;
    case 0xDC: return 6;
    case 0x20: case 0x60: case 0x6B: return 6;
    case 0x22: return 8;
    case 0x54: return 7;
    default: return 0;
    }
}

std::uint8_t Snes65c816TraceCpu::read8(const std::uint8_t bank,
                                        const std::uint16_t address) noexcept {
    timing_.cpu_cycle(bus_clocks(bank, address));
    ++bus_accesses_;
    synchronize_apu();
    if (error_ != Error::none) return 0;
    if (bank == 0x7E || bank == 0x7F) {
        return wram_[(static_cast<unsigned>(bank - 0x7E) << 16) | address];
    }
    const bool system_bank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
    if (system_bank && address < 0x2000) return wram_[address];
    if (system_bank && address >= 0x2140 && address <= 0x2143) {
        return apu_.host_read_port(address - 0x2140);
    }
    if (system_bank && address == 0x213F) return timing_.stat78();
    if (system_bank && address == 0x4210) return timing_.rdnmi();
    if (system_bank && address == 0x4016 && (timing_.hvbjoy() & 1U) == 0)
        return 0; // No controller attached: both data lines are low.
    if (system_bank && address == 0x4017 && (timing_.hvbjoy() & 1U) == 0)
        return 0x1C; // No controller data; fixed bits 2..4 are high.
    if (system_bank && address == 0x4211 && (interrupt_enable_ & 0x30U) == 0)
        return 0;
    if (system_bank && address == 0x4212 && timing_.autojoy_known())
        return timing_.hvbjoy();
    if (system_bank && address >= 0x4218 && address <= 0x421F &&
        timing_.autojoy_known() && (timing_.hvbjoy() & 1U) == 0)
        return 0; // Explicit no-button controller profile, after auto-read.
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
    timing_.cpu_cycle(bus_clocks(bank, address));
    ++bus_accesses_;
    synchronize_apu();
    if (error_ != Error::none) return;
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
    if (system_bank && address == 0x2133) {
        timing_.write_overscan(value);
        return;
    }
    if (system_bank && address == 0x4201) {
        timing_.write_latch(value);
        return;
    }
    if (system_bank && address == 0x420D) {
        fast_rom_ = (value & 1U) != 0;
        return;
    }
    if (system_bank && address == 0x4200) {
        interrupt_enable_ = value;
        nmi_was_enabled_ |= (value & 0x80U) != 0;
        timing_.write_autojoy(value);
        return;
    }
    if (system_bank && (address == 0x6001 || address == 0x6003 ||
                        (address >= 0x6004 && address <= 0x6007))) {
        // Write-only ICD control and joypad forwarding. GB-side effects are
        // not synthesized; reads from this region still fail closed.
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

void Snes65c816TraceCpu::adc(const std::uint16_t value) noexcept {
    const unsigned mask = accumulator_8() ? 0xFFU : 0xFFFFU;
    const unsigned sign = accumulator_8() ? 0x80U : 0x8000U;
    const unsigned a = r_.a & mask;
    const unsigned sum = a + value + ((r_.p & carry) != 0 ? 1U : 0U);
    r_.p = static_cast<std::uint8_t>((r_.p & ~(carry | overflow)) |
        (sum > mask ? carry : 0) |
        ((~(a ^ value) & (a ^ sum) & sign) != 0 ? overflow : 0));
    r_.a = static_cast<std::uint16_t>((r_.a & ~mask) | (sum & mask));
    if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(r_.a));
    else set_nz16(r_.a);
}

void Snes65c816TraceCpu::cmp(const std::uint16_t value) noexcept {
    const auto a = accumulator_8() ? (r_.a & 0xFFU) : r_.a;
    const auto difference = static_cast<std::uint16_t>(a - value);
    if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(difference));
    else set_nz16(difference);
    r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
        (a >= value ? carry : 0));
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
    branch_taken_ = take;
    if (take) {
        const auto previous = r_.pc;
        r_.pc = static_cast<std::uint16_t>(r_.pc + displacement);
        branch_crossed_ = (previous & 0xFF00U) != (r_.pc & 0xFF00U);
    }
}

Snes65c816TraceCpu::StepResult Snes65c816TraceCpu::step() noexcept {
    if (error_ != Error::none) return {error_, 0, r_.pb, r_.pc, error_address_};
    bus_accesses_ = 0;
    branch_taken_ = false;
    branch_crossed_ = false;
    indexed_extra_ = false;
    const bool memory8 = accumulator_8();
    const bool index8 = index_8();
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
    case 0x29: { // AND #imm
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00) |
                (static_cast<std::uint8_t>(r_.a) & fetch8()));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a &= fetch16();
            set_nz16(r_.a);
        }
        break;
    }
    case 0x09: { // ORA #imm
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>(r_.a | fetch8());
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a |= fetch16();
            set_nz16(r_.a);
        }
        break;
    }
    case 0x49: { // EOR #imm
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>(r_.a ^ fetch8());
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a ^= fetch16();
            set_nz16(r_.a);
        }
        break;
    }
    case 0x0D: { // ORA abs
        const auto address = fetch16();
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>(r_.a |
                read8(r_.db, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a |= read16(r_.db, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0x19: { // ORA abs,Y
        const auto base = fetch16();
        const auto address = static_cast<std::uint16_t>(base + r_.y);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>(r_.a | read8(r_.db, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a |= read16(r_.db, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0x2D: { // AND abs
        const auto address = fetch16();
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                (static_cast<std::uint8_t>(r_.a) & read8(r_.db, address)));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a &= read16(r_.db, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0xC9: { // CMP #imm
        cmp(accumulator_8() ? fetch8() : fetch16());
        break;
    }
    case 0xC5: { // CMP dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        cmp(accumulator_8() ? read8(0, address) : read16(0, address));
        break;
    }
    case 0xCD: { // CMP abs
        const auto address = fetch16();
        cmp(accumulator_8() ? read8(r_.db, address) : read16(r_.db, address));
        break;
    }
    case 0x69: { // ADC #imm (binary mode only in this bounded trace)
        if ((r_.p & decimal) != 0) {
            r_.pc = pc;
            return {Error::unsupported_opcode, opcode, bank, pc,
                    (static_cast<std::uint32_t>(bank) << 16) | pc};
        }
        adc(accumulator_8() ? fetch8() : fetch16());
        break;
    }
    case 0x65: { // ADC dp
        if ((r_.p & decimal) != 0) {
            r_.pc = pc;
            return {Error::unsupported_opcode, opcode, bank, pc,
                    (static_cast<std::uint32_t>(bank) << 16) | pc};
        }
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        adc(accumulator_8() ? read8(0, address) : read16(0, address));
        break;
    }
    case 0x7F: { // ADC long,X
        if ((r_.p & decimal) != 0) {
            r_.pc = pc;
            return {Error::unsupported_opcode, opcode, bank, pc,
                    (static_cast<std::uint32_t>(bank) << 16) | pc};
        }
        const auto base = static_cast<std::uint32_t>(fetch16()) |
            (static_cast<std::uint32_t>(fetch8()) << 16);
        const auto address = (base + r_.x) & 0xFFFFFFU;
        const auto low = read8(static_cast<std::uint8_t>(address >> 16),
                               static_cast<std::uint16_t>(address));
        if (accumulator_8()) adc(low);
        else {
            const auto next = (address + 1) & 0xFFFFFFU;
            adc(static_cast<std::uint16_t>(low | (static_cast<unsigned>(read8(
                static_cast<std::uint8_t>(next >> 16),
                static_cast<std::uint16_t>(next))) << 8)));
        }
        break;
    }
    case 0x5F: { // EOR long,X
        const auto base = static_cast<std::uint32_t>(fetch16()) |
            (static_cast<std::uint32_t>(fetch8()) << 16);
        const auto address = (base + r_.x) & 0xFFFFFFU;
        const auto low = read8(static_cast<std::uint8_t>(address >> 16),
                               static_cast<std::uint16_t>(address));
        if (accumulator_8()) {
            r_.a ^= low;
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            const auto next = (address + 1) & 0xFFFFFFU;
            r_.a ^= static_cast<std::uint16_t>(low | (static_cast<unsigned>(read8(
                static_cast<std::uint8_t>(next >> 16),
                static_cast<std::uint16_t>(next))) << 8));
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
    case 0xA0: { // LDY #imm
        r_.y = index_8() ? fetch8() : fetch16();
        if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
        else set_nz16(r_.y);
        break;
    }
    case 0xA4: { // LDY dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        r_.y = index_8() ? read8(0, address) : read16(0, address);
        if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
        else set_nz16(r_.y);
        break;
    }
    case 0xAC: { // LDY abs
        const auto address = fetch16();
        r_.y = index_8() ? read8(r_.db, address) : read16(r_.db, address);
        if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
        else set_nz16(r_.y);
        break;
    }
    case 0x8C: { // STY abs
        const auto address = fetch16();
        if (index_8()) write8(r_.db, address, static_cast<std::uint8_t>(r_.y));
        else write16(r_.db, address, r_.y);
        break;
    }
    case 0x8E: { // STX abs
        const auto address = fetch16();
        if (index_8()) write8(r_.db, address, static_cast<std::uint8_t>(r_.x));
        else write16(r_.db, address, r_.x);
        break;
    }
    case 0xAE: { // LDX abs
        const auto address = fetch16();
        r_.x = index_8() ? read8(r_.db, address) : read16(r_.db, address);
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
    case 0x9F: { // STA long,X
        const auto base = static_cast<std::uint32_t>(fetch16()) |
            (static_cast<std::uint32_t>(fetch8()) << 16);
        const auto address = (base + r_.x) & 0xFFFFFFU;
        write8(static_cast<std::uint8_t>(address >> 16),
               static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(r_.a));
        if (!accumulator_8()) {
            const auto next = (address + 1) & 0xFFFFFFU;
            write8(static_cast<std::uint8_t>(next >> 16),
                   static_cast<std::uint16_t>(next), static_cast<std::uint8_t>(r_.a >> 8));
        }
        break;
    }
    case 0x85: { // STA dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) write8(0, address, static_cast<std::uint8_t>(r_.a));
        else write16(0, address, r_.a);
        break;
    }
    case 0xE6: { // INC dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) {
            const auto old = read8(0, address);
            write8(0, address, old); // RMW dummy write
            const auto value = static_cast<std::uint8_t>(old + 1);
            write8(0, address, value);
            set_nz8(value);
        } else {
            const auto value = static_cast<std::uint16_t>(read16(0, address) + 1);
            write16(0, address, value);
            set_nz16(value);
        }
        break;
    }
    case 0xC6: { // DEC dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) {
            const auto old = read8(0, address);
            write8(0, address, old); // RMW dummy write
            const auto value = static_cast<std::uint8_t>(old - 1);
            write8(0, address, value);
            set_nz8(value);
        } else {
            const auto value = static_cast<std::uint16_t>(read16(0, address) - 1);
            write16(0, address, value);
            set_nz16(value);
        }
        break;
    }
    case 0xEE: { // INC abs
        const auto address = fetch16();
        if (accumulator_8()) {
            const auto old = read8(r_.db, address);
            write8(r_.db, address, old);
            const auto value = static_cast<std::uint8_t>(old + 1);
            write8(r_.db, address, value);
            set_nz8(value);
        } else {
            const auto value = static_cast<std::uint16_t>(read16(r_.db, address) + 1);
            write16(r_.db, address, value);
            set_nz16(value);
        }
        break;
    }
    case 0x9C: { // STZ abs
        const auto address = fetch16();
        if (accumulator_8()) write8(r_.db, address, 0);
        else write16(r_.db, address, 0);
        break;
    }
    case 0x9E: { // STZ abs,X
        const auto address = static_cast<std::uint16_t>(fetch16() + r_.x);
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
    case 0x64: { // STZ dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
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
    case 0xA5: { // LDA dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                read8(0, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = read16(0, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0xB9: { // LDA abs,Y
        const auto base = fetch16();
        const auto address = static_cast<std::uint16_t>(base + r_.y);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                read8(r_.db, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = read16(r_.db, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0xB1: { // LDA (dp),Y
        const auto pointer = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto base = read16(0, pointer);
        const auto address = static_cast<std::uint16_t>(base + r_.y);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                read8(r_.db, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = read16(r_.db, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0xB7: { // LDA [dp],Y
        const auto pointer = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto low = read8(0, pointer);
        const auto high = read8(0, static_cast<std::uint16_t>(pointer + 1));
        const auto pbank = read8(0, static_cast<std::uint16_t>(pointer + 2));
        const auto address = ((static_cast<std::uint32_t>(pbank) << 16) |
            low | (static_cast<std::uint32_t>(high) << 8)) + r_.y;
        const auto effective = address & 0xFFFFFFU;
        const auto value = read8(static_cast<std::uint8_t>(effective >> 16),
                                 static_cast<std::uint16_t>(effective));
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) | value);
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            const auto next = (effective + 1) & 0xFFFFFFU;
            r_.a = static_cast<std::uint16_t>(value | (static_cast<unsigned>(read8(
                static_cast<std::uint8_t>(next >> 16),
                static_cast<std::uint16_t>(next))) << 8));
            set_nz16(r_.a);
        }
        break;
    }
    case 0xA7: { // LDA [dp]
        const auto pointer = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto low = read8(0, pointer);
        const auto high = read8(0, static_cast<std::uint16_t>(pointer + 1));
        const auto pbank = read8(0, static_cast<std::uint16_t>(pointer + 2));
        const auto address = (static_cast<std::uint32_t>(pbank) << 16) |
            low | (static_cast<std::uint32_t>(high) << 8);
        const auto value = read8(static_cast<std::uint8_t>(address >> 16),
                                 static_cast<std::uint16_t>(address));
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) | value);
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            const auto next = (address + 1) & 0xFFFFFFU;
            r_.a = static_cast<std::uint16_t>(value | (static_cast<unsigned>(read8(
                static_cast<std::uint8_t>(next >> 16),
                static_cast<std::uint16_t>(next))) << 8));
            set_nz16(r_.a);
        }
        break;
    }
    case 0x97: { // STA [dp],Y
        const auto pointer = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto low = read8(0, pointer);
        const auto high = read8(0, static_cast<std::uint16_t>(pointer + 1));
        const auto pbank = read8(0, static_cast<std::uint16_t>(pointer + 2));
        const auto address = (((static_cast<std::uint32_t>(pbank) << 16) |
            low | (static_cast<std::uint32_t>(high) << 8)) + r_.y) & 0xFFFFFFU;
        write8(static_cast<std::uint8_t>(address >> 16),
               static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(r_.a));
        if (!accumulator_8()) {
            const auto next = (address + 1) & 0xFFFFFFU;
            write8(static_cast<std::uint8_t>(next >> 16),
                   static_cast<std::uint16_t>(next), static_cast<std::uint8_t>(r_.a >> 8));
        }
        break;
    }
    case 0xBD: { // LDA abs,X
        const auto base = fetch16();
        const auto address = static_cast<std::uint16_t>(base + r_.x);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                read8(r_.db, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = read16(r_.db, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0xBF: { // LDA long,X
        const auto base = static_cast<std::uint32_t>(fetch16()) |
            (static_cast<std::uint32_t>(fetch8()) << 16);
        const auto address = (base + r_.x) & 0xFFFFFFU;
        const auto low = read8(static_cast<std::uint8_t>(address >> 16),
                               static_cast<std::uint16_t>(address));
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) | low);
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            const auto next = (address + 1) & 0xFFFFFFU;
            r_.a = static_cast<std::uint16_t>(low | (static_cast<unsigned>(read8(
                static_cast<std::uint8_t>(next >> 16),
                static_cast<std::uint16_t>(next))) << 8));
            set_nz16(r_.a);
        }
        break;
    }
    case 0xAF: { // LDA long
        const auto address = fetch16();
        const auto bank_byte = fetch8();
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                read8(bank_byte, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = read16(bank_byte, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0x48: { // PHA
        if (!accumulator_8()) push8(static_cast<std::uint8_t>(r_.a >> 8));
        push8(static_cast<std::uint8_t>(r_.a));
        break;
    }
    case 0x08: push8(r_.p); break; // PHP
    case 0x8B: push8(r_.db); break; // PHB
    case 0x28: r_.p = pop8(); set_index_width(); break; // PLP
    case 0x68: { // PLA
        const auto low = pop8();
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) | low);
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a = static_cast<std::uint16_t>(low |
                (static_cast<unsigned>(pop8()) << 8));
            set_nz16(r_.a);
        }
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
    case 0xEB: r_.a = static_cast<std::uint16_t>((r_.a << 8) | (r_.a >> 8));
               set_nz8(static_cast<std::uint8_t>(r_.a)); break; // XBA
    case 0xA8: r_.y = index_8() ? static_cast<std::uint8_t>(r_.a) : r_.a;
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
               else set_nz16(r_.y); break; // TAY
    case 0xAA: r_.x = index_8() ? static_cast<std::uint8_t>(r_.a) : r_.a;
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x); break; // TAX
    case 0x8A: r_.a = accumulator_8() ?
                   static_cast<std::uint16_t>((r_.a & 0xFF00U) | (r_.x & 0xFFU)) :
                   r_.x;
               if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(r_.a));
               else set_nz16(r_.a); break; // TXA
    case 0x98: r_.a = accumulator_8() ?
                   static_cast<std::uint16_t>((r_.a & 0xFF00U) | (r_.y & 0xFFU)) :
                   r_.y;
               if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(r_.a));
               else set_nz16(r_.a); break; // TYA
    case 0xE8: r_.x = static_cast<std::uint16_t>(
                   index_8() ? (r_.x + 1) & 0xFF : r_.x + 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x); break; // INX
    case 0xCA: r_.x = static_cast<std::uint16_t>(
                   index_8() ? (r_.x - 1) & 0xFF : r_.x - 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x); break; // DEX
    case 0xC8: r_.y = static_cast<std::uint16_t>(
                   index_8() ? (r_.y + 1) & 0xFF : r_.y + 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
               else set_nz16(r_.y); break; // INY
    case 0x88: r_.y = static_cast<std::uint16_t>(
                   index_8() ? (r_.y - 1) & 0xFF : r_.y - 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
               else set_nz16(r_.y); break; // DEY
    case 0x3A: { // DEC A
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                ((r_.a - 1) & 0xFFU));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            --r_.a;
            set_nz16(r_.a);
        }
        break;
    }
    case 0x4A: { // LSR A
        const auto old = accumulator_8() ? (r_.a & 0xFFU) : r_.a;
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) | (old & 1U));
        r_.a = static_cast<std::uint16_t>((r_.a & (accumulator_8() ? 0xFF00U : 0U)) |
            (old >> 1));
        if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(r_.a));
        else set_nz16(r_.a);
        break;
    }
    case 0x0A: { // ASL A
        const unsigned mask = accumulator_8() ? 0xFFU : 0xFFFFU;
        const unsigned top = accumulator_8() ? 0x80U : 0x8000U;
        const auto old = r_.a & mask;
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
            ((old & top) != 0 ? carry : 0));
        r_.a = static_cast<std::uint16_t>((r_.a & ~mask) | ((old << 1) & mask));
        if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(r_.a));
        else set_nz16(r_.a);
        break;
    }
    case 0x2E: { // ROL abs
        const auto address = fetch16();
        const auto old = accumulator_8() ? read8(r_.db, address) :
            read16(r_.db, address);
        const unsigned mask = accumulator_8() ? 0xFFU : 0xFFFFU;
        const unsigned top = accumulator_8() ? 0x80U : 0x8000U;
        const auto result = static_cast<std::uint16_t>(
            ((old << 1) | ((r_.p & carry) != 0 ? 1U : 0U)) & mask);
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
            ((old & top) != 0 ? carry : 0));
        if (accumulator_8()) {
            write8(r_.db, address, static_cast<std::uint8_t>(old));
            write8(r_.db, address, static_cast<std::uint8_t>(result));
            set_nz8(static_cast<std::uint8_t>(result));
        } else {
            write16(r_.db, address, result);
            set_nz16(result);
        }
        break;
    }
    case 0x26: { // ROL dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto old = accumulator_8() ? read8(0, address) : read16(0, address);
        const unsigned mask = accumulator_8() ? 0xFFU : 0xFFFFU;
        const unsigned top = accumulator_8() ? 0x80U : 0x8000U;
        const auto result = static_cast<std::uint16_t>(
            ((old << 1) | ((r_.p & carry) != 0 ? 1U : 0U)) & mask);
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
            ((old & top) != 0 ? carry : 0));
        if (accumulator_8()) {
            write8(0, address, static_cast<std::uint8_t>(old));
            write8(0, address, static_cast<std::uint8_t>(result));
            set_nz8(static_cast<std::uint8_t>(result));
        } else {
            write16(0, address, result);
            set_nz16(result);
        }
        break;
    }
    case 0xE0: { // CPX #imm
        const auto value = index_8() ? fetch8() : fetch16();
        const auto difference = static_cast<std::uint16_t>(r_.x - value);
        if (index_8()) set_nz8(static_cast<std::uint8_t>(difference));
        else set_nz16(difference);
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
            (r_.x >= value ? carry : 0));
        break;
    }
    case 0x30: branch((r_.p & negative) != 0); break; // BMI
    case 0x90: branch((r_.p & carry) == 0); break; // BCC
    case 0x10: branch((r_.p & negative) == 0); break; // BPL
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
    case 0x54: { // MVN: encoded destination bank, then source bank
        const auto destination = fetch8();
        const auto source = fetch8();
        const auto value = read8(source, r_.x);
        write8(destination, r_.y, value);
        r_.db = destination;
        ++r_.x;
        ++r_.y;
        --r_.a;
        if (r_.a != 0xFFFFU) r_.pc = pc;
        break;
    }
    case 0xDC: { // JML [abs], 24-bit pointer in bank zero
        const auto pointer = fetch16();
        const auto low = read8(0, pointer);
        const auto high = read8(0, static_cast<std::uint16_t>(pointer + 1));
        r_.pb = read8(0, static_cast<std::uint16_t>(pointer + 2));
        r_.pc = static_cast<std::uint16_t>(low |
            (static_cast<unsigned>(high) << 8));
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
    case 0x22: { // JSL long
        const auto target = fetch16();
        const auto target_bank = fetch8();
        const auto return_address = static_cast<std::uint16_t>(r_.pc - 1);
        push8(r_.pb);
        push8(static_cast<std::uint8_t>(return_address >> 8));
        push8(static_cast<std::uint8_t>(return_address));
        r_.pb = target_bank;
        r_.pc = target;
        break;
    }
    case 0x60: { // RTS
        const auto low = pop8();
        r_.pc = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(pop8()) << 8)) + 1);
        break;
    }
    case 0x6B: { // RTL
        const auto low = pop8();
        const auto high = pop8();
        r_.pb = pop8();
        r_.pc = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + 1);
        break;
    }
    default:
        r_.pc = pc;
        return {Error::unsupported_opcode, opcode, bank, pc,
                (static_cast<std::uint32_t>(bank) << 16) | pc};
    }
    if (error_ != Error::none) return {error_, opcode, bank, pc, error_address_};
    const auto cycles = instruction_cycles(opcode, memory8, index8,
                                           branch_taken_, branch_crossed_);
    if (cycles > bus_accesses_) timing_.cpu_cycle((cycles - bus_accesses_) * 6);
    synchronize_apu();
    if (error_ != Error::none) return {error_, opcode, bank, pc, error_address_};
    ++steps_;
    return {Error::none, opcode, bank, pc, 0};
}

} // namespace sgb_test
