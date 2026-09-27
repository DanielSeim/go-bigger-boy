#pragma once

#include "gameboy/snes_audio_host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace sgb_test {

// Development-only, deliberately incomplete SNES CPU. Every unsupported
// instruction or control-flow-relevant I/O read traps. No firmware is bundled.
class Snes65c816TraceCpu final {
public:
    enum class Error { none, unsupported_opcode, unsupported_read,
                       unsupported_write, trace_overflow };
    struct StepResult {
        Error error{};
        std::uint8_t opcode{};
        std::uint8_t bank{};
        std::uint16_t pc{};
        std::uint32_t address{};
    };
    struct ApuWrite {
        std::uint64_t step{};
        std::uint8_t port{};
        std::uint8_t value{};
    };
    struct Registers {
        std::uint16_t pc{};
        std::uint16_t a{};
        std::uint16_t x{};
        std::uint16_t y{};
        std::uint16_t s{0x01FF};
        std::uint16_t d{};
        std::uint8_t pb{};
        std::uint8_t db{};
        std::uint8_t p{0x34};
        bool e{true};
    };

    Snes65c816TraceCpu(const gameboy::SgbProgramRom& rom,
                        gameboy::SnesApuBus& apu) noexcept;
    [[nodiscard]] StepResult step() noexcept;
    [[nodiscard]] const Registers& registers() const noexcept { return r_; }
    [[nodiscard]] std::size_t apu_write_count() const noexcept { return apu_write_count_; }
    [[nodiscard]] ApuWrite apu_write(std::size_t index) const noexcept {
        return index < apu_write_count_ ? apu_writes_[index] : ApuWrite{};
    }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }

private:
    [[nodiscard]] std::uint8_t read8(std::uint8_t bank,
                                      std::uint16_t address) noexcept;
    void write8(std::uint8_t bank, std::uint16_t address,
                std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t fetch8() noexcept;
    [[nodiscard]] std::uint16_t fetch16() noexcept;
    [[nodiscard]] std::uint16_t read16(std::uint8_t bank,
                                        std::uint16_t address) noexcept;
    void write16(std::uint8_t bank, std::uint16_t address,
                 std::uint16_t value) noexcept;
    void push8(std::uint8_t value) noexcept;
    [[nodiscard]] std::uint8_t pop8() noexcept;
    void set_nz8(std::uint8_t value) noexcept;
    void set_nz16(std::uint16_t value) noexcept;
    void set_index_width() noexcept;
    void branch(bool take) noexcept;
    [[nodiscard]] bool accumulator_8() const noexcept;
    [[nodiscard]] bool index_8() const noexcept;

    const gameboy::SgbProgramRom& rom_;
    gameboy::SnesApuBus& apu_;
    Registers r_{};
    std::array<std::uint8_t, 0x20000> wram_{};
    std::array<ApuWrite, 4096> apu_writes_{};
    std::size_t apu_write_count_{};
    std::uint64_t steps_{};
    Error error_{Error::none};
    std::uint32_t error_address_{};
};

} // namespace sgb_test
