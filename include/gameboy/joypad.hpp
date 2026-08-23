#pragma once

#include <cstdint>

namespace gameboy {

class SaveStateCodec;

enum class Button {
    right,
    left,
    up,
    down,
    a,
    b,
    select,
    start,
};

class Joypad {
public:
    [[nodiscard]] std::uint8_t read() const noexcept;

    // Return true when a selected input line transitions high-to-low.
    [[nodiscard]] bool write(std::uint8_t value) noexcept;
    [[nodiscard]] bool set_button(Button button, bool pressed) noexcept;

private:
    friend class SaveStateCodec;

    [[nodiscard]] std::uint8_t input_lines() const noexcept;

    std::uint8_t select_ = 0x30;
    std::uint8_t directions_{};
    std::uint8_t actions_{};
};

} // namespace gameboy
