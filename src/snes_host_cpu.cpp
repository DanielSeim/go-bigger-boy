#include "gameboy/snes_host_cpu.hpp"

#include "gameboy/snes_spc700.hpp"

#include <algorithm>

namespace gameboy {
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

unsigned SnesHostTiming::line_length() const noexcept {
    return field_ && line_ == 240 ? 1360U : 1364U;
}

void SnesHostTiming::advance(std::uint64_t master_clocks) noexcept {
    // Most bus accesses stay within the current scanline. Boundary clocks
    // retain the original loop, including short lines, NMI and field wrap.
    if (master_clocks < line_length() - horizontal_clock_) {
        horizontal_clock_ += static_cast<unsigned>(master_clocks);
        clocks_ += master_clocks;
        return;
    }
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

void SnesHostTiming::cpu_cycle(unsigned master_clocks) noexcept {
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

std::uint8_t SnesHostTiming::hvbjoy() const noexcept {
    const auto vblank_start = overscan_ ? 240U : 225U;
    const auto auto_start = autojoy_start();
    const bool busy = autojoy_enabled_ && autojoy_known() &&
        clocks_ >= auto_start && clocks_ < auto_start + 4224;
    return static_cast<std::uint8_t>((line_ >= vblank_start ? 0x80 : 0) |
        (horizontal_clock_ >= 1096 || horizontal_clock_ < 4 ? 0x40 : 0) |
        (busy ? 1 : 0));
}

bool SnesHostTiming::autojoy_known() const noexcept {
    if (!autojoy_enabled_ || frames_ == 0) return true;
    const auto latest = frame_start_clocks_ +
        static_cast<std::uint64_t>(overscan_ ? 240U : 225U) * 1364 + 382;
    return autojoy_start() <= latest;
}

std::uint64_t SnesHostTiming::autojoy_start() const noexcept {
    const auto vblank_start = overscan_ ? 240U : 225U;
    const auto first = static_cast<std::uint64_t>(vblank_start) * 1364 + 298;
    if (frames_ == 0) return first;
    // After the first-frame H=74.5-dot start, the controller clock chooses
    // the unique 256-master-clock phase within H=32.5..95.5 dots.
    const auto earliest = frame_start_clocks_ +
        static_cast<std::uint64_t>(vblank_start) * 1364 + 130;
    return earliest + (first + 256 - (earliest % 256)) % 256;
}

std::uint8_t SnesHostTiming::stat78() noexcept {
    const auto result = static_cast<std::uint8_t>(
        0x02 | (field_ ? 0x80 : 0) | (latched_ ? 0x40 : 0));
    if (latch_enable_) latched_ = false;
    return result;
}

std::uint8_t SnesHostTiming::rdnmi() noexcept {
    const auto result = static_cast<std::uint8_t>(0x02 | (nmi_latched_ ? 0x80 : 0));
    nmi_latched_ = false;
    return result;
}

void SnesHostTiming::write_latch(const std::uint8_t value) noexcept {
    const bool enabled = (value & 0x80) != 0;
    if (latch_enable_ && !enabled) latched_ = true;
    latch_enable_ = enabled;
}

bool SnesHostTiming::software_latch() noexcept {
    if (!latch_enable_) return false;
    latched_ = true;
    return true;
}

SnesHostCpu::SnesHostCpu(
    const gameboy::SgbProgramRom& rom, gameboy::SnesApuBus& apu,
    gameboy::SnesSpc700* spc)
    : rom_(rom), apu_(apu), spc_(spc) {
    r_.pc = rom.reset_vector();
}

void SnesHostCpu::synchronize_apu() noexcept {
    if (spc_ == nullptr || error_ != Error::none) return;
    // NTSC master oscillator (1.89e9/88 Hz) versus the 1.024 MHz S-SMP.
    // Completed SPC accesses win an equal-clock tie with the host access.
    // The optional fractional path uses half-clock targets instead. Neither
    // convention claims independently verified hardware port visibility.
    constexpr std::uint64_t master_hz = 21'477'273;
    const std::uint64_t spc_hz = apu_clock_hz_;
    if (fractional_apu_sync_) {
        const auto target_half = timing_.clocks() * (spc_hz * 2) / master_hz;
        if (spc_->half_cycles() >= target_half) return;
        if (apu_batch_driver_ && apu_batch_enabled_) {
            const auto result = apu_batch_driver_(apu_batch_context_, target_half);
            spc_cycles_ = result.completed_cycles;
            if (!result.supported || spc_->half_cycles() != target_half) {
                error_ = Error::unsupported_spc_opcode; error_address_ = result.pc;
            }
            return;
        }
        while (spc_->half_cycles() < target_half) {
            const auto pc = spc_->registers().pc;
            const auto before = spc_->half_cycles();
            if (apu_half_driver_) {
                if (!apu_half_driver_(apu_half_context_) || spc_->half_cycles() != before + 1) {
                    error_ = Error::unsupported_spc_opcode; error_address_ = pc; return;
                }
                spc_cycles_ = spc_->cycles();
                continue;
            }
            const auto result = spc_->clock_half();
            if (!result.instruction.supported || spc_->half_cycles() != before + 1) {
                error_ = Error::unsupported_spc_opcode; error_address_ = pc; return;
            }
            spc_cycles_ = spc_->cycles();
            if (result.completed && spc_step_observer_)
                spc_step_observer_(spc_step_context_, spc_cycles_,
                                   result.instruction.opcode, result.instruction.cycles);
        }
        return;
    }
    const auto target = timing_.clocks() * spc_hz / master_hz;
    while (spc_cycles_ < target) {
        const auto pc = spc_->registers().pc;
        if (cycle_apu_sync_) {
            const auto result = spc_->clock();
            if (!result.instruction.supported || spc_->cycles() != spc_cycles_ + 1) {
                error_ = Error::unsupported_spc_opcode;
                error_address_ = pc;
                return;
            }
            ++spc_cycles_;
            if (result.completed && spc_step_observer_)
                spc_step_observer_(spc_step_context_, spc_cycles_,
                                   result.instruction.opcode, result.instruction.cycles);
            continue;
        }
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

void SnesHostCpu::update_irq() noexcept {
    const auto now = timing_.clocks();
    const auto mode = interrupt_enable_ & 0x30U;
    if (mode == 0 || irq_v_target_ >= 262) {
        last_irq_clock_ = now;
        return;
    }
    // This bounded host trace currently uses V/HV timer IRQs. H-only and
    // external IRQs remain unmodeled and are rejected at $4211.
    if (mode == 0x10U) { last_irq_clock_ = now; return; }
    const auto event_for = [&](const std::uint64_t frame_start,
                               const bool odd_field) {
        const auto line_start = frame_start +
            static_cast<std::uint64_t>(irq_v_target_) * 1364U -
            (odd_field && irq_v_target_ > 240 ? 4U : 0U);
        const auto h = mode == 0x20U ? 0U : irq_h_target_;
        return line_start + (h == 0 ? 10U : 14U + h * 4U);
    };
    if (!irq_cache_valid_ || irq_cache_frame_ != timing_.frame_start_clocks()) {
        irq_cache_frame_ = timing_.frame_start_clocks();
        irq_cache_current_ = event_for(irq_cache_frame_, timing_.field());
        irq_cache_previous_ = 0;
        if (timing_.frames() != 0) {
            const auto previous_start = irq_cache_frame_ -
                (timing_.field() ? 262U * 1364U : 262U * 1364U - 4U);
            irq_cache_previous_ = event_for(previous_start, !timing_.field());
        }
        irq_cache_valid_ = true;
    }
    const auto current = irq_cache_current_;
    if (current > last_irq_clock_ && current <= now) irq_latched_ = true;
    if (timing_.frames() != 0) {
        const auto previous = irq_cache_previous_;
        if (previous > last_irq_clock_ && previous <= now) irq_latched_ = true;
    }
    last_irq_clock_ = now;
}

void SnesHostCpu::service_ppu_dma(unsigned resumed_bus_clocks) noexcept {
    if (!pending_ppu_dma_ || error_ != Error::none) return;
    const auto mask = pending_ppu_dma_;
    pending_ppu_dma_ = 0;
    // Timing-only PPU DMA. Neither PPU data reads nor pixels are modeled.
    // No forward source with read side effects is accepted.
    const auto clock = [this](unsigned amount) {
        timing_.cpu_cycle(amount);
        update_irq();
        synchronize_apu();
    };
    unsigned dma_clocks = 8U - static_cast<unsigned>(timing_.clocks() % 8U);
    clock(dma_clocks);
    clock(8); // Global DMA setup.
    dma_clocks += 8;
    for (unsigned channel = 0; channel < 8 && error_ == Error::none; ++channel) {
        if (!(mask & (1U << channel))) continue;
        const auto base = channel * 16U;
        clock(8); // Per-channel setup.
        dma_clocks += 8;
        auto address = static_cast<std::uint16_t>(dma_registers_[base + 2] |
            (unsigned(dma_registers_[base + 3]) << 8));
        const unsigned size = dma_registers_[base + 5] |
            (unsigned(dma_registers_[base + 6]) << 8);
        const unsigned count = size ? size : 65536U;
        const auto mode = dma_registers_[base];
        for (unsigned i = 0; i < count && error_ == Error::none; ++i) {
            clock(8); // DMA bus slots are always eight clocks, not CPU bus speed.
            if (!(mode & 8)) address += (mode & 16) ? -1 : 1;
        }
        dma_clocks += count * 8U;
        dma_registers_[base + 2] = static_cast<std::uint8_t>(address);
        dma_registers_[base + 3] = static_cast<std::uint8_t>(address >> 8);
        dma_registers_[base + 5] = dma_registers_[base + 6] = 0;
    }
    clock(resumed_bus_clocks - dma_clocks % resumed_bus_clocks);
}

void SnesHostCpu::rmw_dummy(std::uint8_t bank, std::uint16_t address, std::uint8_t old) noexcept {
    if (!host_bus_timing_) { write8(bank, address, old); return; }
    timing_.cpu_cycle(6);
    ++bus_accesses_;
    ++cpu_cycles_;
    update_irq();
    synchronize_apu();
}

unsigned SnesHostCpu::bus_clocks(const std::uint8_t bank,
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

unsigned SnesHostCpu::instruction_cycles(
    const std::uint8_t opcode, const bool memory8, const bool index8,
    const bool branch_taken, const bool branch_crossed) const noexcept {
    const unsigned m = memory8 ? 0U : 1U;
    const unsigned x = index8 ? 0U : 1U;
    const unsigned dp = (r_.d & 0xFFU) == 0 ? 0U : 1U;
    switch (opcode) {
    case 0x18: case 0x38: case 0x58: case 0x78: case 0xD8: case 0xFB:
    case 0x9A: case 0xE8: case 0xC8: case 0xCA: case 0x88:
    case 0x0A: case 0x1A: case 0x3A: case 0x4A: case 0x6A:
    case 0x98: case 0xA8:
    case 0xAA: case 0x8A: case 0x9B: case 0xBB: return 2;
    case 0xC2: case 0xE2: return 3;
    case 0xEB: return 3;
    case 0x09: case 0x29: case 0x49: case 0x69: case 0x89:
    case 0xC9: case 0xE9: case 0xA9:
        return 2 + m;
    case 0xA0: case 0xA2: case 0xC0: case 0xE0: return 2 + x;
    case 0xAC: return 4 + x;
    case 0x8C: case 0x8E: case 0xAE: return 4 + x;
    case 0x0D: case 0x2D: case 0x8D: case 0x9C: case 0xAD: case 0xCD:
    case 0x6D: case 0xED:
        return 4 + m;
    case 0x19: case 0x7D: case 0xB9: case 0xBD: case 0xD9: case 0xDD:
        return 4 + m + (indexed_extra_ ? 1U : 0U);
    case 0xBC: return 4 + x + (indexed_extra_ ? 1U : 0U);
    case 0x11: case 0xB1:
        return 5 + m + dp + (indexed_extra_ ? 1U : 0U);
    case 0x17: case 0x97: case 0xB7: return 6 + m + dp;
    case 0x91: return 6 + m + dp;
    case 0xA7: return 6 + m + dp;
    case 0x5F: case 0x7F: case 0x8F: case 0x9F: case 0xAF: case 0xBF:
        return 5 + m;
    case 0x2E: return 6 + 2 * m;
    case 0x06: case 0x26: case 0x46: return 5 + 2 * m + dp;
    case 0x99: case 0x9D: case 0x9E: return 5 + m;
    case 0x05: case 0x25: case 0x45: case 0x64: case 0x65:
    case 0x85: case 0xA5:
    case 0xC5: case 0xE5:
        return 3 + m + dp;
    case 0xA4: case 0xA6: return 3 + x + dp;
    case 0xC6: case 0xE6: return 5 + 2 * m + dp;
    case 0x04: return 5 + 2 * m + dp;
    case 0xCE: case 0xEE: return 6 + 2 * m;
    case 0xFE: return 7 + 2 * m;
    case 0x74: return 4 + m + dp;
    case 0x48: return 3 + m;
    case 0x5A: case 0xDA: return 3 + x;
    case 0x08: case 0x8B: return 3;
    case 0x68: return 4 + m;
    case 0x7A: case 0xFA: return 4 + x;
    case 0x28: return 4;
    case 0xAB: return 4;
    case 0x2B: return 5;
    case 0x10: case 0x30: case 0x90: case 0xB0: case 0xD0: case 0xF0:
        return 2 + (branch_taken ? 1U : 0U) +
            (r_.e && branch_crossed ? 1U : 0U);
    case 0x80: return 3 + (r_.e && branch_crossed ? 1U : 0U);
    case 0x4C: return 3;
    case 0x5C: return 4;
    case 0x7C: case 0xDC: return 6;
    case 0x20: case 0x60: case 0x6B: return 6;
    case 0x40: return r_.e ? 6U : 7U;
    case 0x22: case 0xFC: return 8;
    case 0x54: return 7;
    default: return 0;
    }
}

std::uint8_t SnesHostCpu::read8(const std::uint8_t bank,
                                       const std::uint16_t address) noexcept {
    const auto value = read8_raw(bank, address);
    if (error_ == Error::none) open_bus_ = value;
    return value;
}

std::uint8_t SnesHostCpu::read8_raw(const std::uint8_t bank,
                                           const std::uint16_t address) noexcept {
    const bool system_bank = (bank & 0x7fU) <= 0x3fU;
    const bool early_apu = fractional_apu_sync_ && system_bank &&
        address >= 0x2140 && address <= 0x2143;
    const auto clocks = bus_clocks(bank, address);
    if (pending_ppu_dma_) service_ppu_dma(clocks);
    timing_.cpu_cycle(clocks - (early_apu ? 4U : 0U));
    update_irq();
    ++bus_accesses_;
    ++cpu_cycles_;
    synchronize_apu();
    if (error_ != Error::none) return 0;
    if (bank == 0x7E || bank == 0x7F) {
        return wram_[(static_cast<unsigned>(bank - 0x7E) << 16) | address];
    }
    // The mapped high half cannot alias any host I/O. Perform the exact
    // bus/APU/IRQ rendezvous above, then bypass lower-half register decoding.
    if (address >= 0x8000) return rom_.read(bank, address);
    if (system_bank && address < 0x2000) return wram_[address];
    if (system_bank && address >= 0x2140 && address <= 0x2143) {
        const auto value = apu_.host_read_port(address - 0x2140);
        if (apu_port_observer_)
            apu_port_observer_(apu_port_context_, timing_.clocks(), 'h', address, value);
        if (early_apu) {
            timing_.cpu_cycle(4);
            update_irq();
            synchronize_apu();
        }
        return value;
    }
    if (system_bank && address == 0x2137) {
        if (timing_.software_latch()) {
            latched_h_ = static_cast<std::uint16_t>(
                timing_.horizontal_clock() / 4U);
            latched_v_ = static_cast<std::uint16_t>(timing_.line());
        }
        return open_bus_; // SLHV is a latch strobe; the data bus is undriven.
    }
    if (system_bank && address == 0x213C) {
        h_counter_high_ = !h_counter_high_;
        return h_counter_high_ ? static_cast<std::uint8_t>(latched_h_) :
            static_cast<std::uint8_t>(latched_h_ >> 8);
    }
    if (system_bank && address == 0x213D) {
        v_counter_high_ = !v_counter_high_;
        return v_counter_high_ ? static_cast<std::uint8_t>(latched_v_) :
            static_cast<std::uint8_t>(latched_v_ >> 8);
    }
    if (system_bank && address == 0x213F) {
        h_counter_high_ = false;
        v_counter_high_ = false;
        return timing_.stat78();
    }
    if (system_bank && address == 0x4210) return timing_.rdnmi();
    if (system_bank && address == 0x4016 && (timing_.hvbjoy() & 1U) == 0)
        return 0; // No controller attached: both data lines are low.
    if (system_bank && address == 0x4017 && (timing_.hvbjoy() & 1U) == 0)
        return 0x1C; // No controller data; fixed bits 2..4 are high.
    if (system_bank && address == 0x4211 &&
        (interrupt_enable_ & 0x30U) != 0x10U) {
        const auto value = static_cast<std::uint8_t>(irq_latched_ ? 0x80U : 0U);
        irq_latched_ = false;
        return value;
    }
    if (system_bank && address == 0x4212 && timing_.autojoy_known())
        return timing_.hvbjoy();
    if (system_bank && address >= 0x4214 && address <= 0x4217) {
        if (math_pending_ && cpu_cycles_ >= math_ready_cycle_) {
            quotient_ = pending_quotient_;
            product_or_remainder_ = pending_product_or_remainder_;
            math_result_valid_ = true;
            if (pending_division_) quotient_valid_ = true;
            math_pending_ = false;
        }
        if (!math_result_valid_ || math_pending_ ||
            (address <= 0x4215 && !quotient_valid_)) {
            error_ = Error::unsupported_read;
            error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
            return 0;
        }
        const auto result = address <= 0x4215 ? quotient_ : product_or_remainder_;
        return static_cast<std::uint8_t>(result >> (8U * (address & 1U)));
    }
    if (system_bank && address >= 0x4218 && address <= 0x421F &&
        timing_.autojoy_known() && (timing_.hvbjoy() & 1U) == 0)
        return 0; // Explicit no-button controller profile, after auto-read.
    if (system_bank && address >= 0x6000 && address <= 0x7FFF) {
        std::uint8_t value{};
        if (icd_ != nullptr && icd_->read(address, timing_.clocks(), value))
            return value;
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
    error_ = Error::unsupported_read;
    error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
    return 0;
}

void SnesHostCpu::write8(const std::uint8_t bank,
                                const std::uint16_t address,
                                const std::uint8_t value) noexcept {
    const auto clocks = bus_clocks(bank, address);
    if (pending_ppu_dma_) service_ppu_dma(clocks);
    timing_.cpu_cycle(clocks);
    update_irq();
    ++bus_accesses_;
    ++cpu_cycles_;
    synchronize_apu();
    if (error_ != Error::none) return;
    open_bus_ = value;
    if (bank == 0x7E || bank == 0x7F) {
        wram_[(static_cast<unsigned>(bank - 0x7E) << 16) | address] = value;
        return;
    }
    const bool system_bank = bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
    if (system_bank && address >= 0x2181 && address <= 0x2183) {
        const unsigned shift = 8U * (address - 0x2181);
        wram_port_address_ = (wram_port_address_ & ~(0xFFU << shift)) |
            (static_cast<unsigned>(value) << shift);
        wram_port_address_ &= 0x1FFFFU;
        return;
    }
    if (system_bank && address == 0x2180) {
        wram_[wram_port_address_] = value;
        wram_port_address_ = (wram_port_address_ + 1U) & 0x1FFFFU;
        return;
    }
    if (system_bank && address >= 0x4300 && address <= 0x437F) {
        dma_registers_[address - 0x4300] = value;
        return;
    }
    if (system_bank && address == 0x420B) {
        ++dma_start_count_;
        last_dma_mask_ = value;
        if (ppu_dma_timing_ && value) {
            // Validate every selected channel before queuing any timing.
            bool ppu_only = true;
            for (unsigned ch = 0; ch < 8; ++ch) {
                if (!(value & (1U << ch))) continue;
                const auto base = ch * 16U;
                if (dma_registers_[base + 1] == 0x80) { ppu_only = false; continue; }
                const auto source_bank = dma_registers_[base + 4];
                const auto source = dma_registers_[base + 2] |
                    (unsigned(dma_registers_[base + 3]) << 8);
                const auto size = dma_registers_[base + 5] |
                    (unsigned(dma_registers_[base + 6]) << 8);
                const auto count = size ? size : 65536U;
                const auto mode = dma_registers_[base];
                const bool wram = source_bank == 0x7E || source_bank == 0x7F;
                const bool fixed = mode & 8;
                const bool mirror = (source_bank <= 0x3F || (source_bank >= 0x80 && source_bank <= 0xBF)) &&
                    source < 0x2000 && (fixed || ((mode & 16) ? source + 1U >= count : 0x2000U - source >= count));
                const bool rom_span = source >= 0x8000 &&
                    (fixed || ((mode & 16) ? source - 0x8000U + 1U >= count : 0x10000U - source >= count));
                const bool reverse = mode & 0x80;
                // Reverse PPU-to-WRAM transfers contribute timing only;
                // PPU read values and destination bytes are not synthesized.
                const bool reverse_supported = reverse && wram;
                if ((mode & 0x60) || dma_registers_[base + 1] > 0x3F ||
                    (reverse ? !reverse_supported : (!wram && !mirror && !rom_span))) {
                    error_ = Error::unsupported_write;
                    error_address_ = address;
                    return;
                }
            }
            if (ppu_only) {
                for (unsigned ch = 0; ch < 8; ++ch)
                    if (value & (1U << ch)) ++dma_destination_counts_[dma_registers_[ch * 16U + 1]];
                pending_ppu_dma_ = value;
                return;
            }
            // Mixing PPU and WRAM DMA is outside this timing-only model.
            for (unsigned ch = 0; ch < 8; ++ch) {
                if ((value & (1U << ch)) && dma_registers_[ch * 16U + 1] != 0x80) {
                    error_ = Error::unsupported_write; error_address_ = address; return;
                }
            }
        }
        for (unsigned channel = 0; channel < 8; ++channel) {
            if ((value & (1U << channel)) != 0) {
                ++dma_destination_counts_[dma_registers_[channel * 16U + 1U]];
                if (dma_registers_[channel * 16U + 1U] == 0x80) {
                    last_wram_dma_target_ = wram_port_address_;
                    for (unsigned offset = 0; offset < 7; ++offset)
                        last_wram_dma_registers_[offset] =
                            dma_registers_[channel * 16U + offset];
                    const auto base = channel * 16U;
                    const auto mode = dma_registers_[base];
                    const auto source = static_cast<std::uint16_t>(
                        dma_registers_[base + 2U] |
                        (static_cast<unsigned>(dma_registers_[base + 3U]) << 8));
                    const auto source_bank = dma_registers_[base + 4U];
                    // This bounded host trace models only the observed ICD
                    // fixed-source byte stream into the WRAM data port.
                    if (mode != 0x08 || source_bank != 0 ||
                        source < 0x7800 || source > 0x780F) {
                        error_ = Error::unsupported_write;
                        error_address_ = (static_cast<std::uint32_t>(bank) << 16) |
                            address;
                        return;
                    }
                    const auto size = static_cast<unsigned>(
                        dma_registers_[base + 5U] |
                        (static_cast<unsigned>(dma_registers_[base + 6U]) << 8));
                    const auto count = size != 0 ? size : 65536U;
                    for (unsigned i = 0; i < count; ++i) {
                        const auto byte = read8(source_bank, source);
                        if (error_ != Error::none) return;
                        wram_[wram_port_address_] = byte;
                        wram_port_address_ = (wram_port_address_ + 1U) & 0x1FFFFU;
                    }
                    dma_registers_[base + 5U] = 0;
                    dma_registers_[base + 6U] = 0;
                }
            }
        }
        return;
    }
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
        if (apu_port_observer_)
            apu_port_observer_(apu_port_context_, timing_.clocks(), 'H', address, value);
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
    if (system_bank && address >= 0x4202 && address <= 0x4206) {
        switch (address) {
        case 0x4202: multiply_a_ = value; break;
        case 0x4203:
            pending_product_or_remainder_ =
                static_cast<std::uint16_t>(multiply_a_ * value);
            pending_quotient_ = quotient_;
            math_ready_cycle_ = cpu_cycles_ + 8;
            math_pending_ = true;
            pending_division_ = false;
            break;
        case 0x4204:
            dividend_ = static_cast<std::uint16_t>((dividend_ & 0xFF00U) | value);
            break;
        case 0x4205:
            dividend_ = static_cast<std::uint16_t>((dividend_ & 0xFFU) |
                (static_cast<unsigned>(value) << 8));
            break;
        case 0x4206:
            pending_quotient_ = value == 0 ? 0xFFFFU :
                static_cast<std::uint16_t>(dividend_ / value);
            pending_product_or_remainder_ = value == 0 ? dividend_ :
                static_cast<std::uint16_t>(dividend_ % value);
            math_ready_cycle_ = cpu_cycles_ + 16;
            math_pending_ = true;
            pending_division_ = true;
            break;
        }
        return;
    }
    if (system_bank && address == 0x4200) {
        interrupt_enable_ = value;
        irq_cache_valid_ = false;
        last_irq_clock_ = timing_.clocks();
        if ((value & 0x30U) == 0) irq_latched_ = false;
        nmi_was_enabled_ |= (value & 0x80U) != 0;
        timing_.write_autojoy(value);
        return;
    }
    if (system_bank && address >= 0x4207 && address <= 0x420A) {
        irq_cache_valid_ = false;
        const auto high = (value & 1U) << 8;
        switch (address) {
        case 0x4207: irq_h_target_ = static_cast<std::uint16_t>(
            (irq_h_target_ & 0x100U) | value); break;
        case 0x4208: irq_h_target_ = static_cast<std::uint16_t>(
            (irq_h_target_ & 0xFFU) | high); break;
        case 0x4209: irq_v_target_ = static_cast<std::uint16_t>(
            (irq_v_target_ & 0x100U) | value); break;
        case 0x420A: irq_v_target_ = static_cast<std::uint16_t>(
            (irq_v_target_ & 0xFFU) | high); break;
        }
        last_irq_clock_ = timing_.clocks();
        return;
    }
    if (system_bank && (address == 0x6001 || address == 0x6003 ||
                        (address >= 0x6004 && address <= 0x6007))) {
        if (icd_ != nullptr &&
            !icd_->write(address, timing_.clocks(), value)) {
            error_ = Error::unsupported_write;
            error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
        }
        // Without an explicit source the legacy bounded trace still accepts
        // these write-only controls, but reads fail closed.
        return;
    }
    // PPU/DMA/CPU registers are write-only in this bounded trace. We permit
    // initialization writes but never fabricate readback or side effects.
    if (system_bank && address >= 0x2000 && address < 0x6000) return;
    error_ = Error::unsupported_write;
    error_address_ = (static_cast<std::uint32_t>(bank) << 16) | address;
}

std::uint8_t SnesHostCpu::fetch8() noexcept {
    const auto value = read8(r_.pb, r_.pc);
    ++r_.pc;
    return value;
}

std::uint16_t SnesHostCpu::fetch16() noexcept {
    const auto low = fetch8();
    return static_cast<std::uint16_t>(low | (static_cast<unsigned>(fetch8()) << 8));
}

std::uint16_t SnesHostCpu::read16(const std::uint8_t bank,
                                          const std::uint16_t address) noexcept {
    const auto low = read8(bank, address);
    return static_cast<std::uint16_t>(low | (static_cast<unsigned>(read8(
        bank, static_cast<std::uint16_t>(address + 1))) << 8));
}

void SnesHostCpu::write16(const std::uint8_t bank,
                                  const std::uint16_t address,
                                  const std::uint16_t value) noexcept {
    write8(bank, address, static_cast<std::uint8_t>(value));
    write8(bank, static_cast<std::uint16_t>(address + 1),
           static_cast<std::uint8_t>(value >> 8));
}

void SnesHostCpu::push8(const std::uint8_t value) noexcept {
    write8(0, r_.s, value);
    r_.s = r_.e ? static_cast<std::uint16_t>(0x0100 | ((r_.s - 1) & 0xFF))
                : static_cast<std::uint16_t>(r_.s - 1);
}

std::uint8_t SnesHostCpu::pop8() noexcept {
    r_.s = r_.e ? static_cast<std::uint16_t>(0x0100 | ((r_.s + 1) & 0xFF))
                : static_cast<std::uint16_t>(r_.s + 1);
    return read8(0, r_.s);
}

void SnesHostCpu::set_nz8(const std::uint8_t value) noexcept {
    r_.p = static_cast<std::uint8_t>((r_.p & ~(negative | zero)) |
        (value & negative) | (value == 0 ? zero : 0));
}

void SnesHostCpu::set_nz16(const std::uint16_t value) noexcept {
    r_.p = static_cast<std::uint8_t>((r_.p & ~(negative | zero)) |
        ((value >> 8) & negative) | (value == 0 ? zero : 0));
}

void SnesHostCpu::adc(const std::uint16_t value) noexcept {
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

void SnesHostCpu::cmp(const std::uint16_t value) noexcept {
    const auto a = accumulator_8() ? (r_.a & 0xFFU) : r_.a;
    const auto difference = static_cast<std::uint16_t>(a - value);
    if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(difference));
    else set_nz16(difference);
    r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
        (a >= value ? carry : 0));
}

void SnesHostCpu::set_index_width() noexcept {
    if (r_.e) r_.p = static_cast<std::uint8_t>(r_.p | memory_width | index_width);
    if (index_8()) {
        r_.x &= 0x00FF;
        r_.y &= 0x00FF;
    }
}

bool SnesHostCpu::accumulator_8() const noexcept {
    return r_.e || (r_.p & memory_width) != 0;
}

bool SnesHostCpu::index_8() const noexcept {
    return r_.e || (r_.p & index_width) != 0;
}

void SnesHostCpu::branch(const bool take) noexcept {
    const auto displacement = static_cast<std::int8_t>(fetch8());
    branch_taken_ = take;
    if (take) {
        const auto previous = r_.pc;
        r_.pc = static_cast<std::uint16_t>(r_.pc + displacement);
        branch_crossed_ = (previous & 0xFF00U) != (r_.pc & 0xFF00U);
    }
}

SnesHostCpu::StepResult SnesHostCpu::step() noexcept {
    if (error_ != Error::none) return {error_, 0, r_.pb, r_.pc, error_address_};
    bus_accesses_ = 0;
    const bool defer_irq = irq_defer_after_cli_;
    irq_defer_after_cli_ = false;
    if (irq_latched_ && (r_.p & irq_disable) == 0 && !defer_irq) {
        if (!r_.e) push8(r_.pb);
        push8(static_cast<std::uint8_t>(r_.pc >> 8));
        push8(static_cast<std::uint8_t>(r_.pc));
        push8(r_.p);
        r_.p = static_cast<std::uint8_t>((r_.p | irq_disable) & ~decimal);
        r_.pb = 0;
        r_.pc = read16(0, r_.e ? 0xFFFE : 0xFFEE);
        ++irq_entries_;
        if (error_ != Error::none)
            return {error_, 0, r_.pb, r_.pc, error_address_};
        bus_accesses_ = 0;
    }
    branch_taken_ = false;
    branch_crossed_ = false;
    indexed_extra_ = false;
    const bool memory8 = accumulator_8();
    const bool index8 = index_8();
    const auto bank = r_.pb;
    const auto pc = r_.pc;
    const auto opcode = fetch8();
    // APU rendezvous can fail during the opcode fetch. Do not turn its
    // sentinel read value into a misleading 65C816 opcode failure.
    if (error_ != Error::none)
        return {error_, opcode, bank, pc, error_address_};
    switch (opcode) {
    case 0x18: r_.p &= static_cast<std::uint8_t>(~carry); break; // CLC
    case 0x38: r_.p |= carry; break; // SEC
    case 0x58: r_.p &= static_cast<std::uint8_t>(~irq_disable);
               irq_defer_after_cli_ = true; break; // CLI
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
    case 0x05: { // ORA dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>(r_.a | read8(0, address));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a |= read16(0, address);
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
    case 0x89: { // BIT #imm changes Z only
        const auto value = accumulator_8() ? fetch8() : fetch16();
        const auto mask = accumulator_8() ? (r_.a & 0xFFU) : r_.a;
        r_.p = static_cast<std::uint8_t>(
            (r_.p & ~zero) | ((value & mask) == 0 ? zero : 0));
        break;
    }
    case 0xC5: { // CMP dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        cmp(accumulator_8() ? read8(0, address) : read16(0, address));
        break;
    }
    case 0x25: { // AND dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                ((r_.a & 0xFFU) & read8(0, address)));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a &= read16(0, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0x45: { // EOR dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                ((r_.a & 0xFFU) ^ read8(0, address)));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a ^= read16(0, address);
            set_nz16(r_.a);
        }
        break;
    }
    case 0xA6: { // LDX dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (index_8()) {
            r_.x = read8(0, address);
            set_nz8(static_cast<std::uint8_t>(r_.x));
        } else {
            r_.x = read16(0, address);
            set_nz16(r_.x);
        }
        break;
    }
    case 0xBC: { // LDY abs,X
        const auto base = fetch16();
        const auto address = static_cast<std::uint16_t>(base + r_.x);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        if (index_8()) {
            r_.y = read8(r_.db, address);
            set_nz8(static_cast<std::uint8_t>(r_.y));
        } else {
            r_.y = read16(r_.db, address);
            set_nz16(r_.y);
        }
        break;
    }
    case 0xCD: { // CMP abs
        const auto address = fetch16();
        cmp(accumulator_8() ? read8(r_.db, address) : read16(r_.db, address));
        break;
    }
    case 0xDD: { // CMP abs,X
        const auto base = fetch16();
        const auto address = static_cast<std::uint16_t>(base + r_.x);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        cmp(accumulator_8() ? read8(r_.db, address) : read16(r_.db, address));
        break;
    }
    case 0xD9: { // CMP abs,Y
        const auto base = fetch16();
        const auto address = static_cast<std::uint16_t>(base + r_.y);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        cmp(accumulator_8() ? read8(r_.db, address) : read16(r_.db, address));
        break;
    }
    case 0x04: { // TSB dp: Z from A & old, then set A bits
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto old = accumulator_8() ? read8(0, address) : read16(0, address);
        const auto mask = accumulator_8() ? (r_.a & 0xFFU) : r_.a;
        r_.p = static_cast<std::uint8_t>(
            (r_.p & ~zero) | ((old & mask) == 0 ? zero : 0));
        if (accumulator_8()) write8(0, address,
            static_cast<std::uint8_t>(old | mask));
        else write16(0, address, static_cast<std::uint16_t>(old | mask));
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
    case 0xE9: { // SBC #imm (binary mode only)
        if ((r_.p & decimal) != 0) {
            r_.pc = pc;
            return {Error::unsupported_opcode, opcode, bank, pc,
                    (static_cast<std::uint32_t>(bank) << 16) | pc};
        }
        const auto value = accumulator_8() ? fetch8() : fetch16();
        adc(static_cast<std::uint16_t>(value ^
            (accumulator_8() ? 0x00FFU : 0xFFFFU)));
        break;
    }
    case 0xED: { // SBC abs (binary mode only)
        if ((r_.p & decimal) != 0) {
            r_.pc = pc;
            return {Error::unsupported_opcode, opcode, bank, pc,
                    (static_cast<std::uint32_t>(bank) << 16) | pc};
        }
        const auto address = fetch16();
        const auto value = accumulator_8() ? read8(r_.db, address) :
            read16(r_.db, address);
        adc(static_cast<std::uint16_t>(value ^
            (accumulator_8() ? 0x00FFU : 0xFFFFU)));
        break;
    }
    case 0xE5: { // SBC dp (binary mode only)
        if ((r_.p & decimal) != 0) {
            r_.pc = pc;
            return {Error::unsupported_opcode, opcode, bank, pc,
                    (static_cast<std::uint32_t>(bank) << 16) | pc};
        }
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto value = accumulator_8() ? read8(0, address) :
            read16(0, address);
        adc(static_cast<std::uint16_t>(value ^
            (accumulator_8() ? 0x00FFU : 0xFFFFU)));
        break;
    }
    case 0x6D: case 0x7D: { // ADC abs / abs,X (binary mode only)
        if ((r_.p & decimal) != 0) {
            r_.pc = pc;
            return {Error::unsupported_opcode, opcode, bank, pc,
                    (static_cast<std::uint32_t>(bank) << 16) | pc};
        }
        const auto base = fetch16();
        const auto address = static_cast<std::uint16_t>(base + (opcode == 0x7D ? r_.x : 0));
        if (opcode == 0x7D)
            indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        adc(accumulator_8() ? read8(r_.db, address) :
            read16(r_.db, address));
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
    case 0x99: { // STA abs,Y
        const auto address = static_cast<std::uint16_t>(fetch16() + r_.y);
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
    case 0x91: { // STA (dp),Y: stores always take the indexing cycle.
        const auto pointer = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto address = static_cast<std::uint16_t>(read16(0, pointer) + r_.y);
        if (accumulator_8()) write8(r_.db, address, static_cast<std::uint8_t>(r_.a));
        else write16(r_.db, address, r_.a);
        break;
    }
    case 0xE6: { // INC dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        if (accumulator_8()) {
            const auto old = read8(0, address);
            rmw_dummy(0, address, old);
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
            rmw_dummy(0, address, old);
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
            rmw_dummy(r_.db, address, old);
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
    case 0xCE: { // DEC abs
        const auto address = fetch16();
        if (accumulator_8()) {
            const auto value = static_cast<std::uint8_t>(
                read8(r_.db, address) - 1U);
            write8(r_.db, address, value);
            set_nz8(value);
        } else {
            const auto value = static_cast<std::uint16_t>(
                read16(r_.db, address) - 1U);
            write16(r_.db, address, value);
            set_nz16(value);
        }
        break;
    }
    case 0xFE: { // INC abs,X
        const auto address = static_cast<std::uint16_t>(fetch16() + r_.x);
        if (accumulator_8()) {
            const auto value = static_cast<std::uint8_t>(
                read8(r_.db, address) + 1U);
            write8(r_.db, address, value);
            set_nz8(value);
        } else {
            const auto value = static_cast<std::uint16_t>(
                read16(r_.db, address) + 1U);
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
    case 0x11: { // ORA (dp),Y
        const auto pointer = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto base = read16(0, pointer);
        const auto address = static_cast<std::uint16_t>(base + r_.y);
        indexed_extra_ = !index_8() || ((base ^ address) & 0xFF00U) != 0;
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                (static_cast<unsigned>(read8(r_.db, address)) |
                 (r_.a & 0xFFU)));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            r_.a |= read16(r_.db, address);
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
    case 0x17: { // ORA [dp],Y
        const auto pointer = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto low = read8(0, pointer);
        const auto high = read8(0, static_cast<std::uint16_t>(pointer + 1));
        const auto pbank = read8(0, static_cast<std::uint16_t>(pointer + 2));
        const auto effective = (((static_cast<std::uint32_t>(pbank) << 16) |
            low | (static_cast<std::uint32_t>(high) << 8)) + r_.y) & 0xFFFFFFU;
        const auto value = read8(static_cast<std::uint8_t>(effective >> 16),
                                 static_cast<std::uint16_t>(effective));
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>(r_.a | value);
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            const auto next = (effective + 1) & 0xFFFFFFU;
            r_.a |= static_cast<std::uint16_t>(value |
                (static_cast<unsigned>(read8(
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
    case 0xDA: { // PHX
        if (!index_8()) push8(static_cast<std::uint8_t>(r_.x >> 8));
        push8(static_cast<std::uint8_t>(r_.x));
        break;
    }
    case 0x5A: { // PHY
        if (!index_8()) push8(static_cast<std::uint8_t>(r_.y >> 8));
        push8(static_cast<std::uint8_t>(r_.y));
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
    case 0x7A: { // PLY
        const auto low = pop8();
        r_.y = index_8() ? low : static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(pop8()) << 8));
        if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
        else set_nz16(r_.y);
        break;
    }
    case 0xFA: { // PLX
        const auto low = pop8();
        r_.x = index_8() ? low : static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(pop8()) << 8));
        if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
        else set_nz16(r_.x);
        break;
    }
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
               else set_nz16(r_.y);
        break; // TAY
    case 0xAA: r_.x = index_8() ? static_cast<std::uint8_t>(r_.a) : r_.a;
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x);
        break; // TAX
    case 0x8A: r_.a = accumulator_8() ?
                   static_cast<std::uint16_t>((r_.a & 0xFF00U) | (r_.x & 0xFFU)) :
                   r_.x;
               if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(r_.a));
               else set_nz16(r_.a);
        break; // TXA
    case 0x98: r_.a = accumulator_8() ?
                   static_cast<std::uint16_t>((r_.a & 0xFF00U) | (r_.y & 0xFFU)) :
                   r_.y;
               if (accumulator_8()) set_nz8(static_cast<std::uint8_t>(r_.a));
               else set_nz16(r_.a);
        break; // TYA
    case 0xE8: r_.x = static_cast<std::uint16_t>(
                   index_8() ? (r_.x + 1) & 0xFF : r_.x + 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x);
        break; // INX
    case 0xCA: r_.x = static_cast<std::uint16_t>(
                   index_8() ? (r_.x - 1) & 0xFF : r_.x - 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x);
        break; // DEX
    case 0xC8: r_.y = static_cast<std::uint16_t>(
                   index_8() ? (r_.y + 1) & 0xFF : r_.y + 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
               else set_nz16(r_.y);
        break; // INY
    case 0x88: r_.y = static_cast<std::uint16_t>(
                   index_8() ? (r_.y - 1) & 0xFF : r_.y - 1);
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
               else set_nz16(r_.y);
        break; // DEY
    case 0xBB: r_.x = index_8() ? (r_.y & 0xFFU) : r_.y;
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.x));
               else set_nz16(r_.x);
               break; // TYX
    case 0x9B: r_.y = index_8() ? (r_.x & 0xFFU) : r_.x;
               if (index_8()) set_nz8(static_cast<std::uint8_t>(r_.y));
               else set_nz16(r_.y);
               break; // TXY
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
    case 0x1A: { // INC A
        if (accumulator_8()) {
            r_.a = static_cast<std::uint16_t>((r_.a & 0xFF00U) |
                ((r_.a + 1) & 0xFFU));
            set_nz8(static_cast<std::uint8_t>(r_.a));
        } else {
            ++r_.a;
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
    case 0x06: { // ASL dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto old = accumulator_8() ? read8(0, address) : read16(0, address);
        const unsigned mask = accumulator_8() ? 0xFFU : 0xFFFFU;
        const unsigned top = accumulator_8() ? 0x80U : 0x8000U;
        const auto result = static_cast<std::uint16_t>((old << 1) & mask);
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
            ((old & top) != 0 ? carry : 0));
        if (accumulator_8()) {
            rmw_dummy(0, address, static_cast<std::uint8_t>(old));
            write8(0, address, static_cast<std::uint8_t>(result));
            set_nz8(static_cast<std::uint8_t>(result));
        } else {
            write16(0, address, result);
            set_nz16(result);
        }
        break;
    }
    case 0x46: { // LSR dp
        const auto address = static_cast<std::uint16_t>(r_.d + fetch8());
        const auto old = accumulator_8() ? read8(0, address) : read16(0, address);
        const auto result = static_cast<std::uint16_t>(old >> 1);
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) | (old & 1U));
        if (accumulator_8()) {
            rmw_dummy(0, address, static_cast<std::uint8_t>(old));
            write8(0, address, static_cast<std::uint8_t>(result));
            set_nz8(static_cast<std::uint8_t>(result));
        } else {
            write16(0, address, result);
            set_nz16(result);
        }
        break;
    }
    case 0x6A: { // ROR A
        const auto old = accumulator_8() ? (r_.a & 0xFFU) : r_.a;
        const auto top = accumulator_8() ? 0x80U : 0x8000U;
        const auto value = static_cast<std::uint16_t>(
            (old >> 1) | ((r_.p & carry) != 0 ? top : 0U));
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) | (old & 1U));
        r_.a = static_cast<std::uint16_t>(
            (r_.a & (accumulator_8() ? 0xFF00U : 0U)) | value);
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
            rmw_dummy(r_.db, address, static_cast<std::uint8_t>(old));
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
            rmw_dummy(0, address, static_cast<std::uint8_t>(old));
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
    case 0xC0: { // CPY #imm
        const auto value = index_8() ? fetch8() : fetch16();
        const auto difference = static_cast<std::uint16_t>(r_.y - value);
        if (index_8()) set_nz8(static_cast<std::uint8_t>(difference));
        else set_nz16(difference);
        r_.p = static_cast<std::uint8_t>((r_.p & ~carry) |
            (r_.y >= value ? carry : 0));
        break;
    }
    case 0x30: branch((r_.p & negative) != 0); break; // BMI
    case 0x90: branch((r_.p & carry) == 0); break; // BCC
    case 0xB0: branch((r_.p & carry) != 0); break; // BCS
    case 0x10: branch((r_.p & negative) == 0); break; // BPL
    case 0xD0: branch((r_.p & zero) == 0); break; // BNE
    case 0xF0: branch((r_.p & zero) != 0); break; // BEQ
    case 0x80: branch(true); break; // BRA
    case 0x4C: r_.pc = fetch16(); break; // JMP abs
    case 0x7C: { // JMP (abs,X), pointer in current program bank
        const auto pointer = static_cast<std::uint16_t>(fetch16() + r_.x);
        const auto low = read8(r_.pb, pointer);
        const auto high = read8(r_.pb,
            static_cast<std::uint16_t>(pointer + 1));
        r_.pc = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        break;
    }
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
    case 0xFC: { // JSR (abs,X), pointer in current program bank
        const auto pointer = static_cast<std::uint16_t>(fetch16() + r_.x);
        const auto low = read8(r_.pb, pointer);
        const auto high = read8(r_.pb,
            static_cast<std::uint16_t>(pointer + 1));
        const auto return_address = static_cast<std::uint16_t>(r_.pc - 1);
        push8(static_cast<std::uint8_t>(return_address >> 8));
        push8(static_cast<std::uint8_t>(return_address));
        r_.pc = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
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
    case 0x40: { // RTI
        r_.p = pop8();
        set_index_width();
        const auto low = pop8();
        const auto high = pop8();
        r_.pc = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        if (!r_.e) r_.pb = pop8();
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
    if (cycles > bus_accesses_) {
        timing_.cpu_cycle((cycles - bus_accesses_) * 6);
        cpu_cycles_ += cycles - bus_accesses_;
        update_irq();
    }
    synchronize_apu();
    if (error_ != Error::none) return {error_, opcode, bank, pc, error_address_};
    ++steps_;
    return {Error::none, opcode, bank, pc, 0};
}

} // namespace gameboy
