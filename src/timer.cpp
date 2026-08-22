#include "gameboy/timer.hpp"

#include <array>

namespace gameboy {

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
    divider_counter_ = 0;
    if (old_signal && !input_signal()) {
        increment_counter();
    }
}

void Timer::write_counter(const std::uint8_t value) noexcept {
    counter_ = value;
    reload_delay_ = 0;
}

void Timer::write_modulo(const std::uint8_t value) noexcept { modulo_ = value; }

void Timer::write_control(const std::uint8_t value) noexcept {
    const auto old_signal = input_signal();
    control_ = static_cast<std::uint8_t>(value & 0x07);
    if (old_signal && !input_signal()) {
        increment_counter();
    }
}

bool Timer::tick(const unsigned cycles) noexcept {
    auto interrupt_requested = false;
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        if (reload_delay_ != 0 && --reload_delay_ == 0) {
            counter_ = modulo_;
            interrupt_requested = true;
        }

        const auto old_signal = input_signal();
        ++divider_counter_;
        if (old_signal && !input_signal()) {
            increment_counter();
        }
    }
    return interrupt_requested;
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
