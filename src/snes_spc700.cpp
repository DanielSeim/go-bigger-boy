#include "gameboy/snes_spc700.hpp"

#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_audio_engine.hpp"

namespace gameboy {
namespace {
constexpr std::uint8_t negative = 0x80;
constexpr std::uint8_t page = 0x20;
constexpr std::uint8_t zero = 0x02;
constexpr std::uint8_t carry = 0x01;
constexpr std::uint8_t half_carry = 0x08;
constexpr std::uint8_t overflow = 0x40;
} // namespace

unsigned SnesSpc700::write_cycle_offset() const noexcept {
    // Offsets include opcode fetch. Word stores and CALL retain separate
    // write boundaries; DBNZ writes before fetching its branch displacement.
    switch (opcode_) {
    case 0x0B: case 0x2B: case 0x4B: case 0x6B: case 0x8B: case 0xAB:
    case 0xC4: case 0xC6: case 0xD8: case 0xCB: case 0xAF: case 0x6E:
        return 4;
    case 0x18: case 0x38: case 0x98: case 0x8F: case 0xD4:
    case 0x9B: case 0xBB: case 0xC5: case 0xC9: case 0xCC:
    case 0x4C: case 0x8C: return 5;
    case 0x09: case 0x0E: case 0x4E: case 0xD5: case 0xD6: return 6;
    case 0xC7: case 0xD7: return 7;
    case 0x2D: case 0x4D: case 0x6D: return 3;
    case 0xDA: return write_index_ == 0 ? 4 : write_index_ == 1 ? 5 : 0;
    case 0x3A: return write_index_ == 0 ? 4 : write_index_ == 1 ? 6 : 0;
    case 0x3F: return write_index_ == 0 ? 5 : write_index_ == 1 ? 6 : 0;
    default:
        return (opcode_ & 0x1FU) == 0x02U || (opcode_ & 0x1FU) == 0x12U ? 4 : 0;
    }
}

void SnesSpc700::clock_bus(const bool early_read) noexcept {
    if (replaying_) {
        if (suspended_ || invalid_replay_) { ++instruction_cycle_; return; }
        if (instruction_cycle_ < replay_count_ &&
            (!half_mode_ || replay_[instruction_cycle_].halves == 2)) {
            ++instruction_cycle_;
            return;
        }
        if (clock_used_) {
            // Only the first blocked access has operands backed entirely by
            // latched reads. Later speculative accesses are not lookahead.
            next_access_early_ = early_read;
            waiting_half_known_ = !half_pending_;
            suspended_ = true; ++instruction_cycle_; return;
        }
        if (instruction_cycle_ == replay_count_ && replay_count_ == replay_.size()) {
            invalid_replay_ = true; ++instruction_cycle_; return;
        }
        if (instruction_cycle_ == replay_count_) {
            replay_[replay_count_++] = {};
            replay_[instruction_cycle_].early_read = early_read;
        }
        clock_used_ = true;
        if (half_mode_) {
            auto& access = replay_[instruction_cycle_++];
            if (access.early_read != early_read) { invalid_replay_ = true; return; }
            ++access.halves;
            half_pending_ = access.halves == 1;
            if (!half_pending_) { bus_.tick(1); ++cycles_; }
            if (half_observer_)
                half_observer_(half_context_, half_cycles(), 'T', 0, 0);
            if (!half_pending_ && dsp_clock_driver_) (void)dsp_clock_driver_->clock();
            if (!half_pending_ && bus_observer_)
                bus_observer_(bus_context_, cycles_, 'T', 0, 0);
            if (half_pending_ && !early_read) suspended_ = true;
            return;
        }
    }
    bus_.tick(1);
    ++instruction_cycle_;
    if (replaying_) ++cycles_;
    if (dsp_clock_driver_) (void)dsp_clock_driver_->clock();
    if (bus_observer_)
        bus_observer_(bus_context_, replaying_ ? cycles_ : cycles_ + instruction_cycle_, 'T', 0, 0);
}

void SnesSpc700::idle_cycle() noexcept {
    clock_bus();
    if (suspended_ || invalid_replay_) return;
    if (replaying_) {
        auto& access = replay_[instruction_cycle_ - 1];
        if (access.kind) {
            if (access.kind != 'I') invalid_replay_ = true;
            return;
        }
        access.kind = 'I';
    }
    if (bus_observer_)
        bus_observer_(bus_context_, replaying_ ? cycles_ : cycles_ + instruction_cycle_, 'I', 0, 0);
    if (replaying_ && half_mode_ && half_observer_)
        half_observer_(half_context_, half_cycles(), 'I', 0, 0);
}

std::uint8_t SnesSpc700::read_memory(const std::uint16_t address) noexcept {
    // A completed latched read has no timer, memory or observer side effect.
    // Internal idle slots have kind I and still take the scheduled slow path.
    // Incomplete midpoint reads must also retain their original rendezvous.
    if (replay_cache_enabled_ && replaying_ && !suspended_ && !invalid_replay_ &&
        instruction_cycle_ < replay_count_) {
        const auto& access = replay_[instruction_cycle_];
        if (access.kind == 'R' && access.address == address &&
            (!half_mode_ || access.halves == 2)) {
            ++instruction_cycle_;
            return access.value;
        }
    }
    if (cycle_bus_) {
        // Address calculation, word arithmetic and branch-bit tests have
        // internal clocks between their operand accesses, not at the tail.
        if (instruction_cycle_ == 2) {
            switch (opcode_) {
            case 0xF4: case 0xFB: case 0xE7: case 0xC7:
            case 0x9B: case 0xBB: case 0xD4: case 0xDE: idle_cycle(); break;
            default: break;
            }
        } else if (instruction_cycle_ == 3) {
            switch (opcode_) {
            case 0xF5: case 0xF6: case 0xB5: case 0xB6: case 0x95:
            case 0x96: case 0x75: case 0xD5: case 0xD6: case 0x1F:
            case 0xBA: case 0x7A: case 0x9A: idle_cycle(); break;
            default:
                if ((opcode_ & 0x1FU) == 3 || (opcode_ & 0x1FU) == 0x13)
                    idle_cycle();
                break;
            }
        } else if (instruction_cycle_ == 4 &&
                   (opcode_ == 0xD7 || opcode_ == 0xF7 || opcode_ == 0xDE)) {
            idle_cycle();
        }
        clock_bus(half_mode_ && (address & 0xfffcU) == 0xf4);
    }
    if (suspended_ || invalid_replay_) return 0;
    if (replaying_) {
        auto& access = replay_[instruction_cycle_ - 1];
        if (access.kind) {
            if (access.kind != 'R' || access.address != address) invalid_replay_ = true;
            return access.value;
        }
    }
    const auto value = bus_.spc_read(address);
    if (replaying_) {
        auto& access = replay_[instruction_cycle_ - 1];
        access.kind = 'R'; access.address = address; access.value = value;
    }
    if (cycle_bus_ && bus_observer_ && !(replaying_ && half_mode_))
        bus_observer_(bus_context_, replaying_ ? cycles_ : cycles_ + instruction_cycle_, 'R', address, value);
    if (replaying_ && half_mode_ && half_observer_)
        half_observer_(half_context_, half_cycles(), 'R', address, value);
    return value;
}

std::uint8_t SnesSpc700::read_load_operand(const std::uint16_t address,
                                         const unsigned cycles) noexcept {
    if (replay_cache_enabled_ && replaying_ && !suspended_ &&
        !invalid_replay_ && !half_pending_) {
        read_tail_known_ = true;
        read_tail_branch_ = false;
        read_tail_registers_ = registers_;
        read_tail_address_ = address;
        read_tail_start_ = instruction_cycle_;
        read_tail_cycles_ = cycles;
        read_tail_opcode_ = opcode_;
    }
    return read_memory(address);
}

void SnesSpc700::begin_bus_instruction() noexcept {
    switch (opcode_) {
    case 0x00: case 0x20: case 0x40: case 0x60: case 0x80: case 0xED:
    case 0x7D: case 0xBD: case 0xDD: case 0xFD: case 0x5D:
    case 0x1C: case 0x5C: case 0x7C: case 0x9F: case 0xCF: case 0x9E:
    case 0x1D: case 0x3D: case 0xBC: case 0xFC: case 0xDC:
    case 0xC6: case 0xE6: case 0xAF:
    case 0x2D: case 0x4D: case 0x6D:
    case 0xAE: case 0xCE: case 0xEE: case 0x6F:
        (void)read_memory(registers_.pc); // Real dummy read; PC does not advance.
        break;
    default: break;
    }
    if (opcode_ == 0xAE || opcode_ == 0xCE || opcode_ == 0xEE || opcode_ == 0x6F)
        idle_cycle();
}

void SnesSpc700::write_memory(const std::uint16_t address,
                              const std::uint8_t value) noexcept {
    const auto offset = write_cycle_offset();
    if (cycle_bus_) {
        switch (opcode_) {
        case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xC9:
        case 0xCB: case 0xCC: case 0xD4: case 0xD5: case 0xD6:
        case 0xD7: case 0xD8: case 0x8F:
            (void)read_memory(address); break; // Store destination dummy read.
        case 0xDA:
            if (write_index_ == 0) (void)read_memory(address);
            break;
        case 0x0E: case 0x4E: (void)read_memory(address); break;
        case 0xAF: idle_cycle(); break;
        case 0x3F:
            if (write_index_ == 0) idle_cycle();
            break;
        default: break;
        }
        clock_bus();
    }
    const auto cycle = replaying_ ? instruction_start_ + instruction_cycle_
                                 : cycle_bus_ ? cycles_ + instruction_cycle_
                                 : offset != 0 ? cycles_ + offset : 0;
    ++write_index_;
    if (suspended_ || invalid_replay_) return;
    if (replaying_) {
        auto& access = replay_[instruction_cycle_ - 1];
        if (access.kind) {
            if (access.kind != 'W' || access.address != address || access.value != value)
                invalid_replay_ = true;
            return;
        }
        access.kind = 'W'; access.address = address; access.value = value;
    }
    if (write_observer_) write_observer_(write_context_, cycle, opcode_, address, value, false);
    bus_.spc_write(address, value);
    if (write_observer_) write_observer_(write_context_, cycle, opcode_, address, value, true);
    if (cycle_bus_ && bus_observer_)
        bus_observer_(bus_context_, cycle, 'W', address, value);
    if (replaying_ && half_mode_ && half_observer_)
        half_observer_(half_context_, half_cycles(), 'W', address, value);
}

std::uint8_t SnesSpc700::fetch() noexcept {
    // Keep the completed operand-prefix path small enough to inline into
    // the interpreter. Generic read_memory handles all new/incomplete
    // accesses, including the first-half input-port latch and dummy clocks.
    if (replay_cache_enabled_ && replaying_ && !suspended_ && !invalid_replay_ &&
        instruction_cycle_ < replay_count_) {
        const auto& access = replay_[instruction_cycle_];
        if (access.kind == 'R' && access.address == registers_.pc &&
            (!half_mode_ || access.halves == 2)) {
            ++instruction_cycle_;
            ++registers_.pc;
            return access.value;
        }
    }
    const auto value = read_memory(registers_.pc);
    ++registers_.pc;
    if (replay_cache_enabled_ && replaying_ && instruction_cycle_ == 2 &&
        !suspended_ && !invalid_replay_ && !half_pending_) {
        switch (opcode_) {
        case 0xE5: case 0xE9: case 0xEC: case 0xF5: case 0xF6:
            absolute_low_ = value;
            absolute_low_known_ = true;
            break;
        default: break;
        }
    }
    return value;
}

std::uint16_t SnesSpc700::direct_address(const std::uint8_t offset) const noexcept {
    return static_cast<std::uint16_t>(
        ((registers_.psw & page) != 0 ? 0x100U : 0U) | offset);
}

std::uint8_t SnesSpc700::read_direct(const std::uint8_t offset) noexcept {
    return read_memory(direct_address(offset));
}

void SnesSpc700::write_direct(const std::uint8_t offset,
                              const std::uint8_t value) noexcept {
    write_memory(direct_address(offset), value);
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
    if (replay_cache_enabled_ && replaying_ && !suspended_ &&
        !invalid_replay_ && !half_pending_) {
        read_tail_known_ = true;
        read_tail_branch_ = true;
        read_tail_take_ = take;
        read_tail_registers_ = registers_;
        read_tail_start_ = instruction_cycle_;
        read_tail_opcode_ = opcode_;
    }
    const auto displacement = static_cast<std::int8_t>(fetch());
    if (take) {
        registers_.pc = static_cast<std::uint16_t>(registers_.pc + displacement);
    }
    return take ? 4U : 2U;
}

void SnesSpc700::push(const std::uint8_t value) noexcept {
    write_memory(static_cast<std::uint16_t>(0x100U | registers_.sp), value);
    --registers_.sp;
}

std::uint8_t SnesSpc700::pop() noexcept {
    return read_memory(static_cast<std::uint16_t>(0x100U | ++registers_.sp));
}

SnesSpc700::StepResult SnesSpc700::step() noexcept {
    if (continuation_) return {0, opcode_, false};
    return execute();
}

SnesSpc700::ClockResult SnesSpc700::clock() noexcept {
    return advance_continuation(false);
}

SnesSpc700::ClockResult SnesSpc700::clock_half() noexcept {
    return advance_continuation(true);
}

SnesSpc700::ClockResult SnesSpc700::advance_continuation(const bool half) noexcept {
    if (!cycle_bus_) return {{0, opcode_, false}, true};
    if (continuation_ && half_mode_ != half) return {{0, opcode_, false}, true};
    if (!continuation_) {
        idle_tail_known_ = false;
        read_tail_known_ = false;
        absolute_low_known_ = false;
        instruction_registers_ = registers_;
        instruction_opcode_ = opcode_;
        instruction_start_ = cycles_;
        replay_count_ = 0;
        continuation_ = true;
        half_mode_ = half;
        // Opcode fetch itself can be an early port read when PC is $00f4..7.
        waiting_half_known_ = true;
        next_access_early_ = (registers_.pc & 0xfffcU) == 0xf4;
    }
    if (half && waiting_half_cache_enabled_ && waiting_half_known_ &&
        !next_access_early_ && !half_pending_ && replay_count_ < replay_.size()) {
        // A normal access does nothing on its first half except report T.
        // Its address/value and all arithmetic are evaluated on the second
        // half by the unchanged interpreter; host writes may occur between.
        waiting_half_known_ = false;
        auto& access = replay_[replay_count_++];
        access = {}; access.halves = 1;
        half_pending_ = clock_used_ = true;
        instruction_cycle_ = replay_count_;
        write_index_ = 0;
        opcode_ = replay_count_ == 1 ? 0 : replay_[0].value;
        if (half_observer_)
            half_observer_(half_context_, half_cycles(), 'T', 0, 0);
        return {{0, opcode_, true}, false};
    }
    waiting_half_known_ = false;
    registers_ = instruction_registers_;
    opcode_ = instruction_opcode_;
    replaying_ = true;
    clock_used_ = suspended_ = invalid_replay_ = false;
    // Replay arithmetic from the last committed registers using latched bus
    // results. After the one new access, remaining work is side-effect-free
    // speculation and is discarded. This avoids a second opcode interpreter,
    // dynamic allocations and an exception/stack unwind at every APU clock.
    StepResult result;
    if (idle_tail_cache_enabled_ && idle_tail_known_) {
        registers_ = idle_tail_registers_;
        opcode_ = replay_[0].value;
        write_index_ = idle_tail_write_index_;
        // Skip only completed, latched prefix accesses. An incomplete idle
        // half must still rendezvous with timers/DSP through clock_bus().
        instruction_cycle_ = replay_count_ - (half_pending_ ? 1U : 0U);
        while (instruction_cycle_ < idle_tail_cycles_) {
            idle_cycle();
            if (suspended_ || invalid_replay_) {
                // Once the next idle access blocks, remaining tail calls
                // only increment this speculative counter. No clocks,
                // accesses, observers or architectural arithmetic occur.
                instruction_cycle_ = idle_tail_cycles_;
                break;
            }
        }
        result = {idle_tail_cycles_, opcode_, true};
    } else if (replay_cache_enabled_ && read_tail_known_) {
        registers_ = read_tail_registers_;
        opcode_ = read_tail_opcode_;
        instruction_cycle_ = read_tail_start_;
        write_index_ = 0;
        if (read_tail_branch_) {
            result = finish_instruction(branch(read_tail_take_));
        } else {
            const auto value = read_memory(read_tail_address_);
            switch (opcode_) {
            case 0xEC: registers_.y = value; break;
            case 0xE9: registers_.x = value; break;
            default: registers_.a = value; break;
            }
            set_nz8(value);
            result = finish_instruction(read_tail_cycles_);
        }
    } else if (replay_cache_enabled_ && absolute_low_known_) {
        // These loads cannot change registers before their target read.
        // Reconstruct the completed prefix instead of saving extra register
        // checkpoints. Only the low byte is reused; both pending reads retain
        // the original fetch/bus/early-port/internal-idle schedule.
        opcode_ = replay_[0].value;
        registers_.pc = static_cast<std::uint16_t>(instruction_registers_.pc + 2U);
        instruction_cycle_ = 2;
        write_index_ = 0;
        const auto high = fetch();
        const auto index = opcode_ == 0xF5 ? registers_.x :
                           opcode_ == 0xF6 ? registers_.y : 0;
        const auto address = static_cast<std::uint16_t>(
            (absolute_low_ | (static_cast<unsigned>(high) << 8)) + index);
        const auto cycles = opcode_ == 0xF5 || opcode_ == 0xF6 ? 5U : 4U;
        const auto value = read_load_operand(address, cycles);
        switch (opcode_) {
        case 0xEC: registers_.y = value; break;
        case 0xE9: registers_.x = value; break;
        default: registers_.a = value; break;
        }
        set_nz8(value);
        result = finish_instruction(cycles);
    } else result = execute();
    replaying_ = false;
    if (invalid_replay_) {
        registers_ = instruction_registers_;
        continuation_ = suspended_ = invalid_replay_ = false;
        return {{0, opcode_, false}, true};
    }
    if (suspended_ || half_pending_) {
        registers_ = instruction_registers_;
        suspended_ = false;
        return {{0, opcode_, true}, false};
    }
    continuation_ = false;
    return {result, true};
}

SnesSpc700::StepResult SnesSpc700::execute() noexcept {
    const auto start = registers_.pc;
    instruction_cycle_ = 0;
    std::uint8_t opcode;
    if (replay_cache_enabled_ && replaying_ && replay_count_ &&
        replay_[0].kind == 'R' && replay_[0].address == start &&
        (!half_mode_ || replay_[0].halves == 2)) {
        // The committed opcode read is invariant throughout a continuation.
        // Bypass generic access validation/dispatch, not the original fetch.
        opcode = replay_[0].value;
        ++registers_.pc; instruction_cycle_ = 1;
    } else opcode = fetch();
    opcode_ = opcode;
    write_index_ = 0;
    if (waiting_half_cache_enabled_ && (suspended_ || invalid_replay_)) return {0, opcode, true};
    if (cycle_bus_) begin_bus_instruction();
    if (waiting_half_cache_enabled_ && (suspended_ || invalid_replay_)) return {0, opcode, true};
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
        const auto source_value = cycle_bus_ ? read_direct(source) : 0;
        const auto destination = fetch();
        const auto value = static_cast<std::uint8_t>(
            read_direct(destination) | (cycle_bus_ ? source_value : read_direct(source)));
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
        subtract_with_carry(read_memory(address));
        cycles = 5;
        break;
    }
    case 0xB6: { // SBC A,!abs+Y
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.y);
        subtract_with_carry(read_memory(address));
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
        add_with_carry(read_memory(address));
        cycles = 5;
        break;
    }
    case 0x85: { // ADC A,!abs
        const auto low = fetch();
        const auto high = fetch();
        add_with_carry(read_memory(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8))));
        cycles = 4;
        break;
    }
    case 0x96: { // ADC A,!abs+Y
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.y);
        add_with_carry(read_memory(address));
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
        const auto old = read_memory(address);
        set_nz8(static_cast<std::uint8_t>(registers_.a - old));
        write_memory(address, static_cast<std::uint8_t>(old | registers_.a));
        cycles = 6;
        break;
    }
    case 0x4E: { // TCLR1 !abs: N/Z from A - old memory, then clear bits
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8));
        const auto old = read_memory(address);
        set_nz8(static_cast<std::uint8_t>(registers_.a - old));
        write_memory(address, static_cast<std::uint8_t>(old & ~registers_.a));
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
        const auto old = read_memory(address);
        const auto value = static_cast<std::uint8_t>(old >> 1);
        registers_.psw = static_cast<std::uint8_t>(
            (registers_.psw & ~carry) | (old & 1U));
        write_memory(address, value);
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
        write_memory(address, registers_.x);
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
        compare(registers_.a, read_memory(static_cast<std::uint16_t>(
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
        compare(registers_.y, read_memory(address));
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
        const auto low = read_direct(offset);
        const auto high = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto rhs = static_cast<unsigned>(low) | (static_cast<unsigned>(high) << 8);
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
        const auto low = read_direct(offset);
        const auto high = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto rhs = static_cast<unsigned>(low) | (static_cast<unsigned>(high) << 8);
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
        if (cycle_bus_) {
            idle_cycle();
            (void)read_memory(static_cast<std::uint16_t>(registers_.pc - 1));
        }
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
        const auto early_displacement = cycle_bus_ ? 0 : fetch();
        const auto value = static_cast<std::uint8_t>(read_direct(offset) - 1U);
        write_direct(offset, value);
        const auto displacement = static_cast<std::int8_t>(
            cycle_bus_ ? fetch() : early_displacement);
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
        const auto early_displacement = cycle_bus_ ? 0 : fetch();
        const auto take = registers_.a != read_direct(offset);
        const auto displacement = static_cast<std::int8_t>(
            cycle_bus_ ? fetch() : early_displacement);
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
        const auto early_source = cycle_bus_ ? read_direct(source) : 0;
        const auto destination = fetch();
        const auto source_value = cycle_bus_ ? early_source : read_direct(source);
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
        registers_.a = read_memory(static_cast<std::uint16_t>(
            lo | (static_cast<unsigned>(hi) << 8)));
        set_nz8(registers_.a);
        cycles = 6;
        break;
    }
    case 0xE5: { // MOV A,!abs
        const auto low = fetch();
        const auto high = fetch();
        registers_.a = read_load_operand(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), 4);
        set_nz8(registers_.a);
        cycles = 4;
        break;
    }
    case 0xEC: { // MOV Y,!abs
        const auto low = fetch();
        const auto high = fetch();
        registers_.y = read_load_operand(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), 4);
        set_nz8(registers_.y);
        cycles = 4;
        break;
    }
    case 0xE9: { // MOV X,!abs
        const auto low = fetch();
        const auto high = fetch();
        registers_.x = read_load_operand(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), 4);
        set_nz8(registers_.x);
        cycles = 4;
        break;
    }
    case 0xF6: { // MOV A,!abs+Y
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.y);
        registers_.a = read_load_operand(address, 5);
        set_nz8(registers_.a);
        cycles = 5;
        break;
    }
    case 0xF5: { // MOV A,!abs+X
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.x);
        registers_.a = read_load_operand(address, 5);
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
        write_memory(address, registers_.a);
        cycles = 6;
        break;
    }
    case 0xD7: {
        const auto offset = fetch();
        const auto lo = read_direct(offset);
        const auto hi = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto address = static_cast<std::uint16_t>(
            (lo | (static_cast<unsigned>(hi) << 8)) + registers_.y);
        write_memory(address, registers_.a);
        cycles = 7;
        break;
    }
    case 0xF7: { // MOV A,[dp]+Y
        const auto offset = fetch();
        const auto lo = read_direct(offset);
        const auto hi = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto address = static_cast<std::uint16_t>(
            (lo | (static_cast<unsigned>(hi) << 8)) + registers_.y);
        registers_.a = read_memory(address);
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
            read_memory(address) - 1U);
        write_memory(address, value);
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
        if (cycle_bus_) write_direct(offset, static_cast<std::uint8_t>(low + 1U));
        const auto high = read_direct(static_cast<std::uint8_t>(offset + 1));
        const auto value = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + 1U);
        if (!cycle_bus_) write_direct(offset, static_cast<std::uint8_t>(value));
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
        write_memory(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), registers_.a);
        cycles = 7;
        break;
    }
    case 0xD5: { // MOV !abs+X,A
        const auto low = fetch();
        const auto high = fetch();
        const auto address = static_cast<std::uint16_t>(
            (low | (static_cast<unsigned>(high) << 8)) + registers_.x);
        write_memory(address, registers_.a);
        cycles = 6;
        break;
    }
    case 0xCC: { // MOV !abs,Y
        const auto low = fetch();
        const auto high = fetch();
        write_memory(static_cast<std::uint16_t>(
            low | (static_cast<unsigned>(high) << 8)), registers_.y);
        cycles = 5;
        break;
    }
    case 0xC5: { // MOV !abs,A
        const auto low = fetch();
        const auto high = fetch();
        write_memory(static_cast<std::uint16_t>(
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
        const auto target_lo = read_memory(address);
        const auto target_hi = read_memory(static_cast<std::uint16_t>(address + 1));
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
        if (cycle_bus_ && !replaying_) cycles_ += instruction_cycle_;
        return {cycle_bus_ ? instruction_cycle_ : 0, opcode, false};
    }
    return finish_instruction(cycles);
}

SnesSpc700::StepResult SnesSpc700::finish_instruction(const unsigned cycles) noexcept {
    if (cycle_bus_) {
        if (instruction_cycle_ > cycles) {
            if (!replaying_) cycles_ += instruction_cycle_;
            return {instruction_cycle_, opcode_, false}; // Reject an invalid bus schedule.
        }
        if (idle_tail_cache_enabled_ && replaying_ && !suspended_ &&
            !invalid_replay_ && !half_pending_ && instruction_cycle_ < cycles) {
            idle_tail_registers_ = registers_;
            idle_tail_cycles_ = cycles;
            idle_tail_write_index_ = write_index_;
            idle_tail_known_ = true;
        }
        while (instruction_cycle_ < cycles) idle_cycle();
    } else {
        bus_.tick(cycles);
    }
    if (!replaying_) cycles_ += cycles;
    return {cycles, opcode_, true};
}

} // namespace gameboy
