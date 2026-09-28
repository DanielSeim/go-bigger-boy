#pragma once

#include "gameboy/snes_audio_host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gameboy { class SnesSpc700; }

namespace sgb_test {

// NTSC, non-interlace timing for the development trace. Master clocks, not
// interpreted instruction counts, drive the observable PPU status bits.
class SnesTraceTiming final {
public:
    void advance(std::uint64_t master_clocks) noexcept;
    void cpu_cycle(unsigned master_clocks) noexcept;
    [[nodiscard]] std::uint8_t hvbjoy() const noexcept;
    [[nodiscard]] std::uint8_t stat78() noexcept;
    [[nodiscard]] std::uint8_t rdnmi() noexcept;
    void write_autojoy(std::uint8_t value) noexcept {
        autojoy_enabled_ = (value & 1U) != 0;
    }
    [[nodiscard]] bool autojoy_known() const noexcept;
    void write_overscan(std::uint8_t value) noexcept { overscan_ = (value & 4U) != 0; }
    void write_latch(std::uint8_t value) noexcept;
    [[nodiscard]] std::uint64_t clocks() const noexcept { return clocks_; }
    [[nodiscard]] unsigned line() const noexcept { return line_; }
    [[nodiscard]] unsigned horizontal_clock() const noexcept { return horizontal_clock_; }
    [[nodiscard]] bool field() const noexcept { return field_; }

private:
    [[nodiscard]] unsigned line_length() const noexcept;
    [[nodiscard]] std::uint64_t autojoy_start() const noexcept;
    std::uint64_t clocks_{};
    unsigned line_{};
    unsigned horizontal_clock_{};
    bool field_{};
    bool overscan_{};
    bool latched_{};
    bool latch_enable_{true};
    bool refresh_done_{};
    bool nmi_latched_{};
    bool autojoy_enabled_{};
    std::uint64_t frames_{};
    std::uint64_t frame_start_clocks_{};
};

// Development-only, deliberately incomplete SNES CPU. Every unsupported
// instruction or control-flow-relevant I/O read traps. No firmware is bundled.
class Snes65c816TraceCpu final {
public:
    enum class Error { none, unsupported_opcode, unsupported_read,
                       unsupported_write, unsupported_spc_opcode, trace_overflow };
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
                        gameboy::SnesApuBus& apu,
                        gameboy::SnesSpc700* spc = nullptr) noexcept;
    [[nodiscard]] StepResult step() noexcept;
    [[nodiscard]] const Registers& registers() const noexcept { return r_; }
    [[nodiscard]] std::size_t apu_write_count() const noexcept { return apu_write_count_; }
    [[nodiscard]] ApuWrite apu_write(std::size_t index) const noexcept {
        return index < apu_write_count_ ? apu_writes_[index] : ApuWrite{};
    }
    void clear_apu_writes() noexcept { apu_write_count_ = 0; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }
    using SpcStepObserver = void (*)(void*, std::uint64_t, std::uint8_t,
                                     unsigned) noexcept;
    void set_spc_step_observer(SpcStepObserver observer,
                               void* context = nullptr) noexcept {
        spc_step_observer_ = observer;
        spc_step_context_ = context;
    }
    [[nodiscard]] std::uint64_t spc_cycles() const noexcept { return spc_cycles_; }
    [[nodiscard]] const SnesTraceTiming& timing() const noexcept { return timing_; }
    [[nodiscard]] std::uint8_t interrupt_enable() const noexcept {
        return interrupt_enable_;
    }
    [[nodiscard]] bool nmi_was_enabled() const noexcept { return nmi_was_enabled_; }

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
    void adc(std::uint16_t value) noexcept;
    void cmp(std::uint16_t value) noexcept;
    void set_index_width() noexcept;
    void branch(bool take) noexcept;
    [[nodiscard]] bool accumulator_8() const noexcept;
    [[nodiscard]] bool index_8() const noexcept;
    [[nodiscard]] unsigned bus_clocks(std::uint8_t bank,
                                      std::uint16_t address) const noexcept;
    [[nodiscard]] unsigned instruction_cycles(std::uint8_t opcode,
                                               bool memory8, bool index8,
                                               bool branch_taken,
                                               bool branch_crossed) const noexcept;
    void synchronize_apu() noexcept;

    const gameboy::SgbProgramRom& rom_;
    gameboy::SnesApuBus& apu_;
    gameboy::SnesSpc700* spc_{};
    std::uint64_t spc_cycles_{};
    SpcStepObserver spc_step_observer_{};
    void* spc_step_context_{};
    Registers r_{};
    std::array<std::uint8_t, 0x20000> wram_{};
    std::array<ApuWrite, 16384> apu_writes_{};
    std::size_t apu_write_count_{};
    std::uint64_t steps_{};
    SnesTraceTiming timing_{};
    unsigned bus_accesses_{};
    bool fast_rom_{};
    std::uint8_t interrupt_enable_{};
    bool nmi_was_enabled_{};
    bool branch_taken_{};
    bool branch_crossed_{};
    bool indexed_extra_{};
    Error error_{Error::none};
    std::uint32_t error_address_{};
};

} // namespace sgb_test
