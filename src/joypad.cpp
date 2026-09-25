#include "gameboy/joypad.hpp"

namespace gameboy {

std::uint8_t Joypad::read() const noexcept {
    return static_cast<std::uint8_t>(0xC0 | select_ | input_lines());
}

std::uint8_t Joypad::read_sgb(const std::uint8_t player_count,
                              const std::uint8_t current_player) const noexcept {
    if (player_count > 1 && select_ == 0x30) {
        return static_cast<std::uint8_t>(0xC0 | select_ |
                                         (0x0F - current_player));
    }
    return static_cast<std::uint8_t>(0xC0 | select_ |
                                     input_lines(current_player));
}

bool Joypad::write(const std::uint8_t value,
                   const std::uint8_t player) noexcept {
    const auto old_lines = input_lines(player);
    select_ = static_cast<std::uint8_t>(value & 0x30);
    const auto new_lines = input_lines(player);
    return (old_lines & static_cast<std::uint8_t>(~new_lines) & 0x0F) != 0;
}

bool Joypad::set_button(const Button button, const bool pressed,
                        const std::uint8_t player) noexcept {
    if (player > 3) return false;
    const auto old_lines = input_lines(player);
    auto* group = player == 0 ? &directions_ : &extra_directions_[player - 1];
    unsigned bit = 0;
    switch (button) {
    case Button::right: bit = 0; break;
    case Button::left: bit = 1; break;
    case Button::up: bit = 2; break;
    case Button::down: bit = 3; break;
    case Button::a:
        group = player == 0 ? &actions_ : &extra_actions_[player - 1];
        bit = 0; break;
    case Button::b:
        group = player == 0 ? &actions_ : &extra_actions_[player - 1];
        bit = 1; break;
    case Button::select:
        group = player == 0 ? &actions_ : &extra_actions_[player - 1];
        bit = 2; break;
    case Button::start:
        group = player == 0 ? &actions_ : &extra_actions_[player - 1];
        bit = 3; break;
    }

    const auto mask = static_cast<std::uint8_t>(1U << bit);
    if (pressed) {
        *group = static_cast<std::uint8_t>(*group | mask);
    } else {
        *group = static_cast<std::uint8_t>(*group & ~mask);
    }
    const auto new_lines = input_lines(player);
    return (old_lines & static_cast<std::uint8_t>(~new_lines) & 0x0F) != 0;
}

std::uint8_t Joypad::input_lines(const std::uint8_t player) const noexcept {
    if (player > 3) return 0x0F;
    const auto directions = player == 0 ? directions_ : extra_directions_[player - 1];
    const auto actions = player == 0 ? actions_ : extra_actions_[player - 1];
    auto lines = std::uint8_t{0x0F};
    if ((select_ & 0x10) == 0) {
        lines = static_cast<std::uint8_t>(lines & ~directions);
    }
    if ((select_ & 0x20) == 0) {
        lines = static_cast<std::uint8_t>(lines & ~actions);
    }
    return static_cast<std::uint8_t>(lines & 0x0F);
}

} // namespace gameboy
