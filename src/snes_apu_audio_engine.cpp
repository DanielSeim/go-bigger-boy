#include "gameboy/snes_apu_audio_engine.hpp"

#include <limits>

namespace gameboy {
SnesApuAudioEngine::SnesApuAudioEngine() noexcept
    : bus_(owned_bus_), cpu_(owned_cpu_) { reset(); }

SnesApuAudioEngine::SnesApuAudioEngine(SnesSpc700& cpu) noexcept
    : bus_(cpu.bus_), cpu_(cpu) { reset(); }

SnesApuAudioEngine::~SnesApuAudioEngine() {
    bus_.set_dsp_write_observer(nullptr);
    cpu_.set_bus_cycle_observer(nullptr);
}

void SnesApuAudioEngine::reset() noexcept {
    bus_.reset();
    bus_.set_dsp_write_observer([](void* context, std::uint8_t address,
                                  std::uint8_t value) noexcept {
        static_cast<SnesApuAudioEngine*>(context)->dsp_.accept_dsp_write(address, value);
    }, this);
    cpu_.reset();
    cpu_.set_cycle_bus_enabled(true);
    // Full-clock T runs after timers but before the accepted SPC access.
    cpu_.set_bus_cycle_observer([](void* context, std::uint64_t, char kind,
                                  std::uint16_t, std::uint8_t) noexcept {
        if (kind == 'T') {
            auto& self = *static_cast<SnesApuAudioEngine*>(context);
            (void)self.dsp_.clock(); // Capacity reserved by clock_half().
        }
    }, this);
    dsp_.reset();
    // Prevent echo DMA from corrupting IPL upload RAM before firmware setup.
    bus_.dsp_publish_register(0x6c, 0xe0);
    dsp_.accept_dsp_write(0x6c, 0xe0);
    status_ = Status::ready;
}

bool SnesApuAudioEngine::clock_half() noexcept {
    if (status_ == Status::unsupported_instruction) return false;
    if (!bus_.has_ipl()) { status_ = Status::missing_ipl; return false; }
    if (dsp_.pending_samples() == SnesDspAudioEngine::buffer_capacity) {
        status_ = Status::buffer_full;
        return false;
    }
    const auto before = cpu_.half_cycles();
    if (before == std::numeric_limits<std::uint64_t>::max()) {
        status_ = Status::invalid_target;
        return false;
    }
    const auto result = cpu_.clock_half();
    if (!result.instruction.supported || cpu_.half_cycles() != before + 1) {
        status_ = Status::unsupported_instruction;
        return false;
    }
    status_ = Status::ready;
    return true;
}

std::uint64_t SnesApuAudioEngine::run_half_clocks(std::uint64_t limit) noexcept {
    std::uint64_t advanced{};
    while (advanced < limit) {
        const auto before = cpu_.half_cycles();
        const bool supported = clock_half();
        advanced += cpu_.half_cycles() - before;
        if (!supported) break;
    }
    return advanced;
}

bool SnesApuAudioEngine::advance_to(std::uint64_t master_clock,
                                   unsigned master_hz, unsigned apu_hz) noexcept {
    if (status_ == Status::unsupported_instruction) return false;
    if (!master_hz || apu_hz < 1'000'000 || apu_hz > 1'100'000 || apu_hz % 32) {
        status_ = Status::invalid_target;
        return false;
    }
    const std::uint64_t half_hz = std::uint64_t(apu_hz) * 2;
    const auto quotient = master_clock / master_hz;
    const auto remainder = (master_clock % master_hz) * half_hz / master_hz;
    if (quotient > (std::numeric_limits<std::uint64_t>::max() - remainder) / half_hz) {
        status_ = Status::invalid_target;
        return false;
    }
    const auto target = quotient * half_hz + remainder;
    const auto current = cpu_.half_cycles();
    if (target < current) { status_ = Status::invalid_target; return false; }
    const auto needed = target - current;
    if (run_half_clocks(needed) != needed) return false;
    if (status_ == Status::unsupported_instruction) return false;
    status_ = Status::ready;
    return true;
}
} // namespace gameboy
