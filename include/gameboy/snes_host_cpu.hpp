#pragma once

#include "gameboy/snes_audio_host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gameboy { class SnesSpc700; class SgbHostStateCodec; }

namespace gameboy {

// Bounded ICD2 input. Returning false leaves host execution stopped at the
// exact access; implementations must not guess missing GB-side data.
class SnesIcdSource {
public:
    virtual ~SnesIcdSource() = default;
    [[nodiscard]] virtual bool read(std::uint16_t address,
                                    std::uint64_t master_clocks,
                                    std::uint8_t& value) noexcept = 0;
    [[nodiscard]] virtual bool write(std::uint16_t address,
                                     std::uint64_t master_clocks,
                                     std::uint8_t value) noexcept = 0;
};

// NTSC, non-interlace timing for the development trace. Master clocks, not
// interpreted instruction counts, drive the observable PPU status bits.
class SnesHostTiming final {
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
    [[nodiscard]] bool software_latch() noexcept;
    [[nodiscard]] std::uint64_t clocks() const noexcept { return clocks_; }
    [[nodiscard]] unsigned line() const noexcept { return line_; }
    [[nodiscard]] unsigned horizontal_clock() const noexcept { return horizontal_clock_; }
    [[nodiscard]] bool field() const noexcept { return field_; }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t frame_start_clocks() const noexcept {
        return frame_start_clocks_;
    }

private:
    friend class SgbHostStateCodec;
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

// Bounded SGB firmware host CPU, deliberately incomplete SNES emulation. Every unsupported
// instruction or control-flow-relevant I/O read traps. No firmware is bundled.
class SnesHostCpu final {
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

    SnesHostCpu(const gameboy::SgbProgramRom& rom,
                        gameboy::SnesApuBus& apu,
                        gameboy::SnesSpc700* spc = nullptr);
    [[nodiscard]] StepResult step() noexcept;
    [[nodiscard]] const Registers& registers() const noexcept { return r_; }
    [[nodiscard]] std::uint8_t debug_wram_byte(std::size_t address) const noexcept {
        return address < wram_.size() ? wram_[address] : 0;
    }
    [[nodiscard]] std::uint64_t dma_start_count() const noexcept {
        return dma_start_count_;
    }
    [[nodiscard]] std::uint8_t last_dma_mask() const noexcept {
        return last_dma_mask_;
    }
    [[nodiscard]] std::uint64_t dma_destination_count(
        std::uint8_t destination) const noexcept {
        return dma_destination_counts_[destination];
    }
    [[nodiscard]] std::uint8_t dma_register(std::uint8_t channel,
                                             std::uint8_t offset) const noexcept {
        return dma_registers_[static_cast<unsigned>(channel & 7U) * 16U +
                              (offset & 15U)];
    }
    [[nodiscard]] std::uint32_t wram_port_address() const noexcept {
        return wram_port_address_;
    }
    [[nodiscard]] std::uint32_t last_wram_dma_target() const noexcept {
        return last_wram_dma_target_;
    }
    [[nodiscard]] std::uint8_t last_wram_dma_register(
        std::uint8_t offset) const noexcept {
        return last_wram_dma_registers_[offset < 7 ? offset : 0];
    }
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
    void set_icd_source(SnesIcdSource* source) noexcept { icd_ = source; }
    using ApuPortObserver = void (*)(void*, std::uint64_t, char,
                                     std::uint16_t, std::uint8_t) noexcept;
    void set_apu_port_observer(ApuPortObserver observer, void* context = nullptr) noexcept {
        apu_port_observer_ = observer; apu_port_context_ = context;
    }
    // Opt-in diagnostic rendezvous; only whole completed SPC clocks run before
    // a host access. The legacy instruction-granular baseline stays unchanged.
    void set_cycle_apu_sync_enabled(bool enabled) noexcept { cycle_apu_sync_ = enabled; }
    // Diagnostic midpoint SPC input reads and SNES reads before a four-clock
    // bus tail. Requires the attached SPC's explicit cycle-bus opt-in.
    void set_fractional_apu_sync_enabled(bool enabled) noexcept {
        fractional_apu_sync_ = enabled;
        if (enabled) cycle_apu_sync_ = true;
    }
    // Diagnostic override used to exercise the reusable core APU scheduler.
    // It must advance the attached SPC by exactly one half clock and drain PCM.
    using ApuHalfDriver = bool (*)(void*) noexcept;
    void set_apu_half_driver(ApuHalfDriver driver, void* context) noexcept {
        apu_half_driver_ = driver; apu_half_context_ = context;
    }
    [[nodiscard]] std::uint64_t spc_cycles() const noexcept { return spc_cycles_; }
    // Capture-only oscillator profile, set before execution. The nominal
    // 1.024 MHz runtime clock is not changed by selecting a reference profile.
    bool set_apu_clock_hz(unsigned hz) noexcept {
        if (hz < 1000000 || hz > 1100000 || hz % 32 || timing_.clocks()) return false;
        apu_clock_hz_ = hz; return true;
    }
    [[nodiscard]] unsigned apu_clock_hz() const noexcept { return apu_clock_hz_; }
    // Correct the legacy eight-bit RMW old-value writes to six-clock idles.
    // Opt-in preserves the historical bounded trace/PCM baselines.
    void set_host_bus_timing_enabled(bool enabled) noexcept { host_bus_timing_ = enabled; }
    // Experimental timing-only PPU DMA path. No SNES PPU pixels are modeled.
    // Historical audio baselines retain their explicitly bounded legacy path.
    void set_ppu_dma_timing_enabled(bool enabled) noexcept { ppu_dma_timing_ = enabled; }
    [[nodiscard]] const SnesHostTiming& timing() const noexcept { return timing_; }
    [[nodiscard]] std::uint8_t interrupt_enable() const noexcept {
        return interrupt_enable_;
    }
    [[nodiscard]] bool nmi_was_enabled() const noexcept { return nmi_was_enabled_; }
    [[nodiscard]] std::uint64_t irq_entries() const noexcept { return irq_entries_; }

private:
    friend class SgbHostStateCodec;
    [[nodiscard]] std::uint8_t read8(std::uint8_t bank,
                                      std::uint16_t address) noexcept;
    [[nodiscard]] std::uint8_t read8_raw(std::uint8_t bank,
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
    void update_irq() noexcept;
    void service_ppu_dma(unsigned resumed_bus_clocks) noexcept;
    void rmw_dummy(std::uint8_t bank, std::uint16_t address, std::uint8_t old) noexcept;

    const gameboy::SgbProgramRom& rom_;
    gameboy::SnesApuBus& apu_;
    gameboy::SnesSpc700* spc_{};
    ApuHalfDriver apu_half_driver_{};
    void* apu_half_context_{};
    bool cycle_apu_sync_{};
    bool fractional_apu_sync_{};
    bool ppu_dma_timing_{};
    bool host_bus_timing_{};
    std::uint8_t pending_ppu_dma_{};
    ApuPortObserver apu_port_observer_{};
    void* apu_port_context_{};
    SnesIcdSource* icd_{};
    std::uint64_t spc_cycles_{};
    unsigned apu_clock_hz_{1024000};
    SpcStepObserver spc_step_observer_{};
    void* spc_step_context_{};
    Registers r_{};
    std::array<std::uint8_t, 0x20000> wram_{};
    std::array<std::uint8_t, 0x80> dma_registers_{};
    std::array<std::uint64_t, 256> dma_destination_counts_{};
    std::array<std::uint8_t, 7> last_wram_dma_registers_{};
    std::uint32_t wram_port_address_{};
    std::uint32_t last_wram_dma_target_{};
    std::uint64_t dma_start_count_{};
    std::uint8_t last_dma_mask_{};
    std::uint8_t multiply_a_{};
    std::uint16_t dividend_{};
    std::uint16_t quotient_{};
    std::uint16_t product_or_remainder_{};
    std::uint16_t pending_quotient_{};
    std::uint16_t pending_product_or_remainder_{};
    std::uint64_t cpu_cycles_{};
    std::uint64_t math_ready_cycle_{};
    bool math_result_valid_{};
    bool quotient_valid_{};
    bool math_pending_{};
    bool pending_division_{};
    // Keep the bounded trace off the stack: tests may hold several CPUs at once
    // and Windows test processes use a smaller default thread stack.
    std::vector<ApuWrite> apu_writes_ = std::vector<ApuWrite>(16384);
    std::size_t apu_write_count_{};
    std::uint64_t steps_{};
    SnesHostTiming timing_{};
    unsigned bus_accesses_{};
    bool fast_rom_{};
    std::uint8_t interrupt_enable_{};
    std::uint16_t irq_h_target_{};
    std::uint16_t irq_v_target_{};
    std::uint64_t last_irq_clock_{};
    bool irq_latched_{};
    bool irq_defer_after_cli_{};
    std::uint64_t irq_entries_{};
    std::uint8_t open_bus_{};
    std::uint16_t latched_h_{};
    std::uint16_t latched_v_{};
    bool h_counter_high_{};
    bool v_counter_high_{};
    bool nmi_was_enabled_{};
    bool branch_taken_{};
    bool branch_crossed_{};
    bool indexed_extra_{};
    Error error_{Error::none};
    std::uint32_t error_address_{};
};

} // namespace gameboy
