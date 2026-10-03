#pragma once

#include "gameboy/sgb_icd_gb_source.hpp"
#include "gameboy/sgb_audio_mixer.hpp"
#include "gameboy/snes_apu_audio_engine.hpp"
#include <memory>

namespace gameboy {

// Caller-owned, legally obtained images only. No filesystem or external core
// dependency. Input events are held at native GB LCD-frame boundaries.
struct SgbHostConfig {
    std::vector<std::uint8_t> program_rom;
    std::vector<std::uint8_t> game_rom;
    std::vector<std::uint8_t> battery_ram; // Optional initial save, never loaded/written automatically.
    DiagnosticBootRom gb_boot_rom{};
    SnesApuBus::IplRom spc_ipl{};
    HardwareModel model{HardwareModel::sgb2};
    unsigned apu_clock_hz{1024000};
    std::vector<SgbIcdGbSource::InputEvent> input_events;
    // Opt-in digital presentation, not hardware-calibrated analog SGB levels.
    bool combined_audio{};
    unsigned output_hz{48000}; // 8000..48000; native SNES mode ignores this rate.
    unsigned gb_gain_q15{16384}, snes_gain_q15{16384}; // 0..32768, provisional 50% each.
};

// Bounded original firmware host, not a full SNES/PPU emulator. This opt-in
// component is not connected to any shipping frontend. Defaults to SNES PCM;
// the explicit combined mode adds GB capture and bounded rate conversion.
// Single-threaded. Mutable processors are deliberately not exposed.
class SgbHost final {
public:
    enum class Status : std::uint8_t { ready, buffer_full, host_fault, apu_fault, icd_fault };
    using StereoSample = SnesApuAudioEngine::StereoSample;
    static constexpr std::size_t buffer_capacity = 16384;
    // Eight maximum-length DMA channels take about 4.2M master clocks. Even
    // at the highest accepted APU frequency they produce fewer than 8192
    // samples. Reserve before starting an instruction, never midway through IO.
    static constexpr std::size_t instruction_reserve = 8192;
    static constexpr std::size_t combined_instruction_reserve = 12000;
    explicit SgbHost(SgbHostConfig config);
    ~SgbHost();
    SgbHost(const SgbHost&) = delete;
    SgbHost& operator=(const SgbHost&) = delete;
    void reset(); // Allocation permitted; rebuilds a cold host with the same images.
    [[nodiscard]] bool step() noexcept;
    [[nodiscard]] std::uint64_t run_until(std::uint64_t master_target,
                                         std::uint64_t instruction_budget) noexcept;
    [[nodiscard]] bool pop_sample(StereoSample& sample) noexcept;
    [[nodiscard]] std::size_t pending_samples() const noexcept;
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] const SnesHostCpu& cpu() const noexcept;
    [[nodiscard]] const SgbIcdGbSource& icd() const noexcept;
    [[nodiscard]] std::uint64_t apu_half_clocks() const noexcept;
    [[nodiscard]] std::uint64_t samples_produced() const noexcept;
    [[nodiscard]] std::uint64_t snes_samples_produced() const noexcept;
    [[nodiscard]] std::uint64_t gb_samples_captured() const noexcept;
    [[nodiscard]] std::uint64_t clipped_samples() const noexcept;
    [[nodiscard]] unsigned sample_rate() const noexcept;
    [[nodiscard]] SnesHostCpu::StepResult fault() const noexcept;
    // Instruction-boundary snapshot: CPU/timing/WRAM/DMA, APU, GB, ICD
    // packet/input/row state and unread host PCM. Private images may be present.
    [[nodiscard]] std::vector<std::uint8_t> save_state() const;
    [[nodiscard]] bool load_state(const std::vector<std::uint8_t>& state) noexcept;
private:
    friend class SgbHostStateCodec;
    struct Impl;
    const SgbHostConfig config_;
    std::unique_ptr<Impl> impl_;
};
} // namespace gameboy
