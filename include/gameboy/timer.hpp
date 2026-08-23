#pragma once

#include <cstdint>

namespace gameboy {

class SaveStateCodec;

class Timer {
public:
    [[nodiscard]] std::uint8_t divider() const noexcept;
    [[nodiscard]] std::uint8_t counter() const noexcept;
    [[nodiscard]] std::uint8_t modulo() const noexcept;
    [[nodiscard]] std::uint8_t control() const noexcept;

    void write_divider() noexcept;
    void write_counter(std::uint8_t value) noexcept;
    void write_modulo(std::uint8_t value) noexcept;
    void write_control(std::uint8_t value) noexcept;

    // Returns true when TIMA reload requests the timer interrupt.
    [[nodiscard]] bool tick(unsigned cycles) noexcept;
    [[nodiscard]] unsigned take_apu_ticks() noexcept;

private:
    friend class SaveStateCodec;

    [[nodiscard]] bool input_signal() const noexcept;
    void increment_counter() noexcept;

    std::uint16_t divider_counter_{};
    std::uint8_t counter_{};
    std::uint8_t modulo_{};
    std::uint8_t control_{};
    unsigned reload_delay_{};
    unsigned apu_ticks_{};
};

} // namespace gameboy
