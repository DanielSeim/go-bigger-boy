#pragma once

#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_audio_engine.hpp"
#include "gameboy/snes_spc700.hpp"

namespace gameboy {

// Original, caller-scheduled S-SMP/S-DSP component. Not a SNES/SGB host or
// frontend audio device. Single threaded; firmware is supplied by the caller.
class SnesApuAudioEngine final {
public:
    using StereoSample = SnesDspAudioEngine::StereoSample;
    enum class Status : std::uint8_t { ready, buffer_full, unsupported_instruction, invalid_target, missing_ipl };
    explicit SnesApuAudioEngine() noexcept;
    // Attach to a caller-owned bus/CPU at reset. Both must outlive the engine;
    // this resets them and reserves their DSP-write/full-clock observers.
    explicit SnesApuAudioEngine(SnesSpc700& cpu) noexcept;
    ~SnesApuAudioEngine();
    SnesApuAudioEngine(const SnesApuAudioEngine&) = delete;
    SnesApuAudioEngine& operator=(const SnesApuAudioEngine&) = delete;

    // Resets CPU, bus and DSP together. Installed IPL is retained by bus reset.
    void reset() noexcept;
    void install_ipl(const SnesApuBus::IplRom& image) noexcept { bus_.install_ipl(image); }
    // Diagnostic/setup access only. Do not replace the DSP-write observer:
    // it is owned by this scheduler. No mutable CPU/DSP is exposed.
    SnesApuBus& bus() noexcept { return bus_; }
    const SnesSpc700& cpu() const noexcept { return cpu_; }
    [[nodiscard]] bool clock_half() noexcept;
    // Actual physical halves advanced, including a terminal unsupported fetch.
    // Inspect status even when the returned count equals the requested limit.
    [[nodiscard]] std::uint64_t run_half_clocks(std::uint64_t limit) noexcept;
    // Absolute master rendezvous, with exact integer conversion and no drift.
    // Retry the same target after draining a full FIFO. Profiles are caller
    // metadata, not hardware detection: runtime default stays 1,024,000 Hz.
    [[nodiscard]] bool advance_to(std::uint64_t master_clock,
                                 unsigned master_hz = 21'477'273,
                                 unsigned apu_hz = 1'024'000) noexcept;
    [[nodiscard]] bool pop_sample(StereoSample& sample) noexcept { return dsp_.pop_sample(sample); }
    [[nodiscard]] std::size_t pending_samples() const noexcept { return dsp_.pending_samples(); }
    [[nodiscard]] Status status() const noexcept { return status_; }
    // Atomic component state includes CPU replay/half clocks, bus, DSP and
    // FIFO. Excludes the SNES CPU, ICD/GB scheduler and frontend DAC/resampler.
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    [[nodiscard]] bool load_state(const std::vector<std::uint8_t>& bytes) noexcept;
private:
    friend class SgbHost;
    friend class SnesDspStateCodec;
    SnesApuBus owned_bus_;
    SnesSpc700 owned_cpu_{owned_bus_};
    SnesApuBus& bus_;
    SnesSpc700& cpu_;
    SnesDspAudioEngine dsp_{bus_};
    Status status_{Status::ready};
};

} // namespace gameboy
