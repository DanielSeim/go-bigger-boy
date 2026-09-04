#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace gameboy {

class SaveStateCodec;
class SaveStateJoypadCodec;
class SaveStateBusCodec;

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

    // Return true when a selected input line transitions high-to-low.
    [[nodiscard]] bool write(std::uint8_t value) noexcept;
    [[nodiscard]] bool set_button(Button button, bool pressed) noexcept;
    void set_sgb_mode(bool enabled) noexcept;
    [[nodiscard]] bool take_sgb_packet(
        std::array<std::uint8_t, sgb_packet_size * sgb_max_packets>& packet,
        std::size_t& size) noexcept;

private:
    friend class SaveStateCodec;
    friend class SaveStateJoypadCodec;
    friend class SaveStateBusCodec;

    [[nodiscard]] std::uint8_t input_lines() const noexcept;
    void process_sgb_write(std::uint8_t value) noexcept;
    void reset_sgb_packet() noexcept;
    [[nodiscard]] std::size_t sgb_command_bits() const noexcept;

    std::uint8_t select_ = 0x30;
    std::uint8_t directions_{};
    std::uint8_t actions_{};
    bool sgb_mode_{};
    bool sgb_ready_for_pulse_{};
    bool sgb_ready_for_write_{};
    bool sgb_ready_for_stop_{};
    std::size_t sgb_bit_count_{};
    std::array<std::uint8_t, sgb_packet_size * sgb_max_packets> sgb_command_{};
    std::array<std::uint8_t, sgb_packet_size * sgb_max_packets> sgb_packet_{};
    bool sgb_packet_ready_{};
    std::size_t sgb_packet_bytes_{};
};

} // namespace gameboy
