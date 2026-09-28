#pragma once

#include <cstdint>

namespace gameboy {

class SnesApuBus;

// Incremental SPC700 interpreter for the IPL and bounded driver probes.
// Unsupported opcodes
// stop explicitly; they are never treated as NOPs or as synthesized audio.
class SnesSpc700 final {
public:
    struct Registers {
        std::uint16_t pc{0xFFC0};
        std::uint8_t a{};
        std::uint8_t x{};
        std::uint8_t y{};
        std::uint8_t sp{};
        std::uint8_t psw{};
    };

    struct StepResult {
        unsigned cycles{};
        std::uint8_t opcode{};
        bool supported{};
    };

    explicit SnesSpc700(SnesApuBus& bus) noexcept : bus_(bus) {}
    void reset() noexcept { registers_ = {}; }
    [[nodiscard]] const Registers& registers() const noexcept { return registers_; }
    [[nodiscard]] StepResult step() noexcept;

private:
    [[nodiscard]] std::uint8_t fetch() noexcept;
    [[nodiscard]] std::uint16_t direct_address(std::uint8_t offset) const noexcept;
    [[nodiscard]] std::uint8_t read_direct(std::uint8_t offset) noexcept;
    void write_direct(std::uint8_t offset, std::uint8_t value) noexcept;
    void set_nz8(std::uint8_t value) noexcept;
    void set_nz16(std::uint16_t value) noexcept;
    void compare(std::uint8_t lhs, std::uint8_t rhs) noexcept;
    void add_with_carry(std::uint8_t rhs) noexcept;
    [[nodiscard]] unsigned branch(bool take) noexcept;
    void push(std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t pop() noexcept;

    SnesApuBus& bus_;
    Registers registers_{};
};

} // namespace gameboy
