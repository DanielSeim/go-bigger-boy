#pragma once

#include "gameboy/snes_dsp_clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gameboy {

// Caller-clocked DSP engine, not an executing SNES/SPC700 host. Single-threaded:
// the caller owns scheduling, synchronization and delivery/resampling to a DAC.
// Frontends do not instantiate this component yet.
class SnesDspAudioEngine final {
public:
    using StereoSample = SnesDspPcmRenderer::StereoSample;
    static constexpr std::size_t buffer_capacity = 512;
    static constexpr unsigned sample_rate = SnesDspPcmRenderer::sample_rate;

    explicit SnesDspAudioEngine(SnesApuBus& bus) noexcept;
    SnesDspAudioEngine(const SnesDspAudioEngine&) = delete;
    SnesDspAudioEngine& operator=(const SnesDspAudioEngine&) = delete;

    // Clear synthesis, phase and queued output, but preserve external APU RAM,
    // programmed registers, timers, IPL and observer registrations. DSP-owned
    // ENVX/OUTX/ENDX readback is cleared to match the reset synthesis pipeline.
    void reset() noexcept;
    void write_dsp(std::uint8_t address, std::uint8_t value) noexcept {
        renderer_.write_dsp(address, value);
    }
    // Commit on the shared bus first; do not also call write_dsp for this write.
    void accept_dsp_write(std::uint8_t address, std::uint8_t value) noexcept {
        renderer_.accept_dsp_write(address, value);
    }
    // Full buffer applies backpressure: false means no phase/state advanced.
    // Never overwrite/drop old audio or allocate on this path.
    [[nodiscard]] bool clock() noexcept;
    [[nodiscard]] std::uint64_t run_clocks(std::uint64_t limit) noexcept;
    [[nodiscard]] bool pop_sample(StereoSample& sample) noexcept;
    [[nodiscard]] std::size_t pending_samples() const noexcept { return size_; }
    [[nodiscard]] unsigned phase() const noexcept { return clock_.phase(); }

    // Versioned little-endian component snapshot, including the shared APU bus
    // and queued PCM. No pointers/callbacks, host padding or firmware paths.
    // Does NOT save an attached SPC700/65C816 CPU: a future composite state
    // must coordinate those separately. Save allocates off the audio path;
    // restore validates everything before changing any live state and retains
    // existing observer registrations. Private firmware/RAM may occur in states.
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    [[nodiscard]] bool load_state(const std::vector<std::uint8_t>& bytes) noexcept;

private:
    friend class SnesDspStateCodec;
    SnesApuBus& bus_;
    SnesDspPcmRenderer renderer_;
    SnesDspClock clock_;
    std::array<StereoSample, buffer_capacity> buffer_{};
    std::uint16_t head_{};
    std::uint16_t size_{};
};

} // namespace gameboy
