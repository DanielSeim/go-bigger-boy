#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace gameboy {

class SaveStateCodec;
class SaveStateJoypadCodec;
class SaveStateBusCodec;
class SgbAdapter;

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
    static constexpr std::size_t sgb_packet_size = 16;
    static constexpr std::size_t sgb_max_packets = 7;

    [[nodiscard]] std::uint8_t read() const noexcept;
    [[nodiscard]] std::uint8_t read_sgb(std::uint8_t player_count,
                                        std::uint8_t current_player) const noexcept;
    [[nodiscard]] std::uint8_t select_lines() const noexcept { return select_; }

    // Return true when a selected input line transitions high-to-low.
    [[nodiscard]] bool write(std::uint8_t value,
                             std::uint8_t player = 0) noexcept;
    [[nodiscard]] bool set_button(Button button, bool pressed,
                                  std::uint8_t player = 0) noexcept;

private:
    friend class SaveStateCodec;
    friend class SaveStateJoypadCodec;
    friend class SaveStateBusCodec;
    friend class SgbAdapter;

    [[nodiscard]] std::uint8_t input_lines(std::uint8_t player = 0) const noexcept;
    std::uint8_t select_ = 0x30;
    std::uint8_t directions_{};
    std::uint8_t actions_{};
    std::array<std::uint8_t, 3> extra_directions_{};
    std::array<std::uint8_t, 3> extra_actions_{};
};

} // namespace gameboy
