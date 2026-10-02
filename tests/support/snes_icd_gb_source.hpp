#pragma once
#include "sgb_frame_input.hpp"

#include "snes_65c816_trace_cpu.hpp"

#include "gameboy/emulator.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <vector>

namespace sgb_test {

// Convert SNES master clocks to GB cycles without truncating at each host
// step. SGB1 shares the SNES oscillator; SGB2 uses a dedicated oscillator.
constexpr std::uint64_t sgb_icd_target_gb_cycles(
    const std::uint64_t master_elapsed, const unsigned divider,
    const gameboy::HardwareModel model) noexcept {
    constexpr std::uint64_t master_hz = 21'477'273ULL;
    const auto oscillator_hz = model == gameboy::HardwareModel::sgb2
        ? 20'971'520ULL : master_hz;
    const auto denominator = master_hz * divider;
    return (master_elapsed / denominator) * oscillator_hz +
           ((master_elapsed % denominator) * oscillator_hz) / denominator;
}

// Test-only live GB input for the bounded SNES trace. The GB clock is anchored
// to ICD reset release; status is scanline-granular, not a cycle-exact ICD2
// model. Unknown row-buffer data deliberately fails closed.
class SnesIcdGbSource final : public SnesIcdTraceSource {
public:
    void set_native_gb_input(bool enabled) noexcept {
        // Configure before release. Native replay executes an external boot
        // from LCD/DIV reset rather than inheriting a post-boot bus image.
        native_gb_input_ = enabled;
        if (enabled) initialize_external_boot_bus();
    }
    [[nodiscard]] std::uint8_t diagnostic_joypad_read() noexcept { return gb_->bus().read8(0xff00); }
    [[nodiscard]] std::uint8_t diagnostic_io_read(std::uint16_t address) noexcept { return gb_->bus().read8(address); }
    using BootObserver = void (*)(void*, char, std::uint64_t, std::uint32_t, std::uint64_t) noexcept;
    void set_boot_observer(BootObserver observer, void* context = nullptr) noexcept {
        boot_observer_ = observer;
        boot_observer_context_ = context;
    }
    explicit SnesIcdGbSource(const std::filesystem::path& rom,
                             const std::filesystem::path& boot_rom,
                             gameboy::HardwareModel model);

    [[nodiscard]] bool read(std::uint16_t address, std::uint64_t master_clocks,
                            std::uint8_t& value) noexcept override;
    [[nodiscard]] bool write(std::uint16_t address, std::uint64_t master_clocks,
                             std::uint8_t value) noexcept override;
    void advance_to(std::uint64_t master_clocks) noexcept {
        synchronize(master_clocks);
    }
    [[nodiscard]] std::uint64_t gb_cycles() const noexcept { return gb_cycles_; }
    [[nodiscard]] std::uint64_t packets_completed() const noexcept {
        return packets_completed_;
    }
    [[nodiscard]] std::uint64_t sound_commands() const noexcept {
        return sound_commands_;
    }
    [[nodiscard]] std::uint64_t packets_delivered() const noexcept {
        return packets_delivered_;
    }
    [[nodiscard]] std::uint64_t sound_packets_delivered() const noexcept {
        return sound_packets_delivered_;
    }
    [[nodiscard]] std::uint64_t audible_sound_packets_delivered() const noexcept {
        return audible_sound_packets_delivered_;
    }
    [[nodiscard]] const std::array<std::uint8_t, 16>& first_sound_packet()
        const noexcept { return first_sound_packet_; }
    [[nodiscard]] const std::array<std::uint8_t, 16>& last_delivered_sound_packet()
        const noexcept { return last_delivered_sound_packet_; }
    [[nodiscard]] std::uint64_t transfer_commands() const noexcept {
        return transfer_commands_;
    }
    [[nodiscard]] std::uint16_t missing_address() const noexcept {
        return missing_address_;
    }
    [[nodiscard]] std::uint64_t control_writes() const noexcept {
        return control_writes_;
    }
    [[nodiscard]] std::uint8_t last_control() const noexcept {
        return last_control_;
    }
    // Synthetic diagnostic only: replace the first GB SOUND payload while
    // preserving the packet's real delivery timing and command framing.
    void set_audible_sound_substitution(bool enabled) noexcept {
        audible_sound_substitution_ = enabled;
    }
    void load_input_script(const std::filesystem::path& path);
    [[nodiscard]] std::uint64_t completed_frames() const noexcept {
        return completed_frames_;
    }
    [[nodiscard]] std::uint64_t input_events_applied() const noexcept {
        return input_events_applied_;
    }
    [[nodiscard]] std::uint64_t audible_sound_commands() const noexcept {
        return audible_sound_commands_;
    }
    [[nodiscard]] const std::array<std::uint8_t, 16>& first_audible_sound_packet()
        const noexcept { return first_audible_sound_packet_; }
    [[nodiscard]] std::uint64_t first_audible_frame() const noexcept {
        return first_audible_frame_;
    }

private:
    void synchronize(std::uint64_t master_clocks) noexcept;
    void initialize_external_boot_bus() noexcept;
    void joyp_write(std::uint8_t value) noexcept;
    void complete_packet() noexcept;
    void complete_tile_row(unsigned tile_row) noexcept;
    void apply_input(std::uint64_t frame) noexcept;

    struct InputEvent {
        unsigned frame{};
        std::uint8_t mask{};
    };

    std::unique_ptr<gameboy::Emulator> gb_;
    gameboy::DiagnosticBootRom boot_image_{};
    std::uint64_t release_clock_{};
    std::uint64_t master_snapshot_{};
    std::uint64_t gb_cycles_{};
    std::uint64_t packets_completed_{};
    std::uint64_t sound_commands_{};
    std::uint64_t packets_delivered_{};
    std::uint64_t sound_packets_delivered_{};
    std::uint64_t audible_sound_packets_delivered_{};
    std::array<std::uint8_t, 16> first_sound_packet_{};
    std::array<std::uint8_t, 16> last_delivered_sound_packet_{};
    std::array<std::uint8_t, 16> first_audible_sound_packet_{};
    std::uint64_t audible_sound_commands_{};
    std::uint64_t first_audible_frame_{};
    std::uint64_t completed_frames_{};
    std::uint64_t input_events_applied_{};
    std::vector<InputEvent> input_events_;
    std::size_t next_input_event_{};
    std::uint8_t held_buttons_{};
    FrameInput frame_input_;
    void set_input_buttons(std::uint8_t mask) noexcept;
    bool native_gb_input_{};
    std::uint64_t transfer_commands_{};
    std::uint16_t missing_address_{};
    std::uint64_t control_writes_{};
    std::uint8_t last_control_{};
    unsigned divider_{5};
    gameboy::HardwareModel model_{};
    bool released_{};
    bool boot_reported_{};
    BootObserver boot_observer_{};
    void* boot_observer_context_{};
    bool pulse_armed_{true};
    bool receiving_{};
    bool packet_pending_{};
    bool audible_sound_substitution_{};
    unsigned bit_count_{};
    unsigned continuation_packets_{};
    unsigned last_ly_{};
    unsigned selected_row_{};
    unsigned row_stream_offset_{};
    std::array<std::array<std::uint8_t, 320>, 4> rows_{};
    std::array<bool, 4> row_valid_{};
    std::array<std::uint8_t, 16> building_{};
    std::array<std::uint8_t, 16> latched_{};
    std::deque<std::array<std::uint8_t, 16>> queued_;
};

} // namespace sgb_test
