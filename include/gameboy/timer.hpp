#pragma once

#include <cstdint>

namespace gameboy {

class SaveStateCodec;
class SaveStateTimerCodec;
class SaveStateBusCodec;
enum class HardwareModel;

class Timer {
public:
    [[nodiscard]] std::uint16_t debug_divider_counter() const noexcept {
        return divider_counter_;
    }
    [[nodiscard]] std::uint8_t divider() const noexcept;
    [[nodiscard]] std::uint8_t counter() const noexcept;
    [[nodiscard]] std::uint8_t modulo() const noexcept;
    [[nodiscard]] std::uint8_t control() const noexcept;

    void write_divider() noexcept;
    void write_counter(std::uint8_t value) noexcept;
    void write_modulo(std::uint8_t value) noexcept;
    void write_control(std::uint8_t value,
                       bool cpu_bus_cycle = false) noexcept;
    void set_double_speed(bool enabled) noexcept;
    void initialize_post_boot(HardwareModel model) noexcept;

    // Returns true when TIMA reload requests the timer interrupt.
    [[nodiscard]] bool tick(unsigned cycles) noexcept;
    [[nodiscard]] unsigned take_apu_ticks() noexcept;
    [[nodiscard]] bool apu_signal() const noexcept;
    [[nodiscard]] unsigned cycles_until_apu_tick() const noexcept {
        const auto mask = double_speed_ ? 0x3fffU : 0x1fffU;
        return apu_ticks_ ? 1U : mask + 1 - (divider_counter_ & mask);
    }
    // CPU bus batching retains the last T-cycle's reload-write lock, unlike
    // the historical standalone tick() which ORs reloads across its span.
    [[nodiscard]] bool tick_bus(unsigned cycles) noexcept;

private:
    [[nodiscard]] bool tick_impl(unsigned cycles, bool last_reload_only) noexcept;
    friend class SaveStateCodec;
    friend class SaveStateTimerCodec;
    friend class SaveStateBusCodec;

    [[nodiscard]] bool input_signal() const noexcept;
    void increment_counter() noexcept;

    std::uint16_t divider_counter_{};
    std::uint8_t counter_{};
    std::uint8_t modulo_{};
    std::uint8_t control_{};
    unsigned reload_delay_{};
    unsigned apu_ticks_{};
    bool reload_happened_{};
    bool double_speed_{};
};

} // namespace gameboy
