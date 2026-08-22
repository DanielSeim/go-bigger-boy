#include "gameboy/joypad.hpp"

namespace gameboy {

std::uint8_t Joypad::read() const noexcept {
    return static_cast<std::uint8_t>(0xC0 | select_ | input_lines());
}

bool Joypad::write(const std::uint8_t value) noexcept {
    const auto old_lines = input_lines();
    select_ = static_cast<std::uint8_t>(value & 0x30);
    const auto new_lines = input_lines();
    return (old_lines & static_cast<std::uint8_t>(~new_lines) & 0x0F) != 0;
}

bool Joypad::set_button(const Button button, const bool pressed) noexcept {
    const auto old_lines = input_lines();
    auto* group = &directions_;
    unsigned bit = 0;
    switch (button) {
    case Button::right: bit = 0; break;
    case Button::left: bit = 1; break;
    case Button::up: bit = 2; break;
    case Button::down: bit = 3; break;
    case Button::a: group = &actions_; bit = 0; break;
    case Button::b: group = &actions_; bit = 1; break;
    case Button::select: group = &actions_; bit = 2; break;
    case Button::start: group = &actions_; bit = 3; break;
    }

    const auto mask = static_cast<std::uint8_t>(1U << bit);
    if (pressed) {
        *group = static_cast<std::uint8_t>(*group | mask);
    } else {
        *group = static_cast<std::uint8_t>(*group & ~mask);
    }
    const auto new_lines = input_lines();
    return (old_lines & static_cast<std::uint8_t>(~new_lines) & 0x0F) != 0;
}

std::uint8_t Joypad::input_lines() const noexcept {
    auto lines = std::uint8_t{0x0F};
    if ((select_ & 0x10) == 0) {
        lines = static_cast<std::uint8_t>(lines & ~directions_);
    }
    if ((select_ & 0x20) == 0) {
        lines = static_cast<std::uint8_t>(lines & ~actions_);
    }
    return static_cast<std::uint8_t>(lines & 0x0F);
}

} // namespace gameboy
