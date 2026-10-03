#include "gameboy/timer.hpp"

#include "gameboy/hardware_model.hpp"

#include <array>

namespace gameboy {

void Timer::initialize_post_boot(const HardwareModel model) noexcept {
    // Boot ROM revisions hand control to the cartridge at different phases of
    // the free-running system divider.
    switch (model) {
    case HardwareModel::dmg0: divider_counter_ = 0x182C; break;
    case HardwareModel::sgb: divider_counter_ = 0xD85C; break;
    case HardwareModel::sgb2: divider_counter_ = 0xD84C; break;
    case HardwareModel::automatic:
    case HardwareModel::dmg:
    case HardwareModel::mgb: divider_counter_ = 0xABC8; break;
    case HardwareModel::cgb0:
    case HardwareModel::cgb_c: divider_counter_ = 0x2880; break;
    case HardwareModel::cgb:
    case HardwareModel::cgb_e: divider_counter_ = 0x2674; break;
    }
}

std::uint8_t Timer::divider() const noexcept {
    return static_cast<std::uint8_t>(divider_counter_ >> 8);
}

std::uint8_t Timer::counter() const noexcept { return counter_; }

std::uint8_t Timer::modulo() const noexcept { return modulo_; }

std::uint8_t Timer::control() const noexcept {
    return static_cast<std::uint8_t>(control_ | 0xF8);
}

void Timer::write_divider() noexcept {
    const auto old_signal = input_signal();
    const auto apu_bit = double_speed_ ? 13U : 12U;
    const auto old_apu_signal = (divider_counter_ & (1U << apu_bit)) != 0;
    divider_counter_ = 0;
    if (old_signal && !input_signal()) {
        increment_counter();
    }
    if (old_apu_signal) ++apu_ticks_;
}

void Timer::write_counter(const std::uint8_t value) noexcept {
    if (!reload_happened_) {
        counter_ = value;
        reload_delay_ = 0;
    }
    reload_happened_ = false;
}

void Timer::write_modulo(const std::uint8_t value) noexcept {
    modulo_ = value;
    if (reload_happened_) counter_ = value;
    reload_happened_ = false;
}

void Timer::write_control(const std::uint8_t value,
                          const bool cpu_bus_cycle) noexcept {
    constexpr std::array<unsigned, 4> divider_bits{9, 3, 5, 7};
    const auto bit = divider_bits[control_ & 0x03];
    // TAC's mux transition reaches the counter after a two-M-cycle hardware
    // propagation window. Sampling that phase is observable during rapid
    // enable/disable sequences even though ordinary timer periods are unchanged.
    const auto sample_counter = static_cast<std::uint16_t>(
        divider_counter_ + (cpu_bus_cycle ? 8 : 0));
    const auto old_signal = (control_ & 0x04) != 0 &&
                            (sample_counter & (1U << bit)) != 0;
    control_ = static_cast<std::uint8_t>(value & 0x07);
    if (old_signal && !input_signal()) {
        increment_counter();
    }
}

void Timer::set_double_speed(const bool enabled) noexcept {
    double_speed_ = enabled;
}

bool Timer::tick(const unsigned cycles) noexcept {
    return tick_impl(cycles, false);
}
bool Timer::tick_bus(const unsigned cycles) noexcept {
    return tick_impl(cycles, true);
}
bool Timer::tick_impl(const unsigned cycles, const bool last_reload_only) noexcept {
    auto interrupt_requested = false;
    reload_happened_ = false;
    constexpr std::array<unsigned, 4> divider_bits{9, 3, 5, 7};
    const auto timer_mask = (1U << (divider_bits[control_ & 3U] + 1)) - 1;
    const auto apu_mask = double_speed_ ? 0x3fffU : 0x1fffU;
    if (cycles < apu_mask + 1 - (divider_counter_ & apu_mask) &&
        (!(control_ & 4U) || cycles < timer_mask + 1 - (divider_counter_ & timer_mask)) &&
        (!reload_delay_ || cycles < reload_delay_)) {
        // No falling edge or reload in this span: only the divider and an
        // optional pending reload countdown change. Boundary cycles retain
        // their original increment-before-compare order below.
        divider_counter_ = static_cast<std::uint16_t>(divider_counter_ + cycles);
        if (reload_delay_) reload_delay_ -= cycles;
        return false;
    }
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        if (last_reload_only) reload_happened_ = false;
        if (reload_delay_ != 0 && --reload_delay_ == 0) {
            counter_ = modulo_;
            reload_happened_ = true;
            interrupt_requested = true;
        }

        ++divider_counter_;
        // A falling divider bit is exactly a wrap of its lower bit field,
        // including the uint16 wrap. No need to evaluate both signals.
        if ((control_ & 4U) && (divider_counter_ & timer_mask) == 0) {
            increment_counter();
        }
        if ((divider_counter_ & apu_mask) == 0) ++apu_ticks_;
    }
    return interrupt_requested;
}

unsigned Timer::take_apu_ticks() noexcept {
    const auto ticks = apu_ticks_;
    apu_ticks_ = 0;
    return ticks;
}

bool Timer::apu_signal() const noexcept {
    const auto bit = double_speed_ ? 13U : 12U;
    return (divider_counter_ & (1U << bit)) != 0;
}

bool Timer::input_signal() const noexcept {
    if ((control_ & 0x04) == 0) {
        return false;
    }
    constexpr std::array<unsigned, 4> divider_bits{9, 3, 5, 7};
    const auto bit = divider_bits[control_ & 0x03];
    return (divider_counter_ & (1U << bit)) != 0;
}

void Timer::increment_counter() noexcept {
    if (reload_delay_ != 0) {
        return;
    }
    if (counter_ == 0xFF) {
        counter_ = 0;
        reload_delay_ = 4;
    } else {
        ++counter_;
    }
}

} // namespace gameboy
