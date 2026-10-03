#pragma once

#include "gameboy/snes_dsp_audio_engine.hpp"

namespace gameboy {

// Experimental digital presentation path, not the SGB analog circuit.
// Timestamped, held DAC values are area-resampled on the SNES master timeline.
// Feed both sources through a target before advancing it. Single-threaded.
class SgbAudioMixer final {
public:
    using StereoSample = SnesDspAudioEngine::StereoSample;
    enum class Source : std::uint8_t { gb, snes };
    static constexpr std::uint64_t master_hz = 21477273;
    static constexpr std::size_t capacity = 16384;
    explicit SgbAudioMixer(unsigned output_hz = 48000,
                           unsigned gb_gain_q15 = 16384,
                           unsigned snes_gain_q15 = 16384);
    // A timestamp is the first master clock at which the DAC value is known.
    // Invalid ordering/full queues fail without changing state.
    [[nodiscard]] bool push(Source source, std::uint64_t master_clock,
                            StereoSample sample) noexcept;
    // Discard speculative GB samples at/after reset and insert silence there.
    [[nodiscard]] bool reset_gb_at(std::uint64_t master_clock) noexcept;
    // Preflights output capacity. Retry after draining; no partial advancement.
    [[nodiscard]] bool advance_to(std::uint64_t master_clock) noexcept;
    [[nodiscard]] bool pop_sample(StereoSample& sample) noexcept;
    [[nodiscard]] std::size_t pending_samples() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t samples_produced() const noexcept { return produced_; }
    [[nodiscard]] std::uint64_t clipped_samples() const noexcept { return clipped_; }
    [[nodiscard]] unsigned sample_rate() const noexcept { return output_hz_; }
private:
    friend class SgbHostStateCodec;
    struct Event { std::uint64_t clock{}; StereoSample sample{}; };
    struct Stream {
        std::array<Event, capacity> events{};
        std::size_t head{}, count{};
        std::uint64_t last_clock{};
        StereoSample held{};
    };
    bool validate() const noexcept;
    void refresh_cache() noexcept;
    unsigned output_hz_, gb_gain_q15_, snes_gain_q15_;
    // Derived from the immutable output rate, never serialized.
    std::uint64_t maximum_sample_clock_{}, maximum_advance_clock_{};
    std::array<Stream, 2> streams_{};
    std::array<StereoSample, capacity> pcm_{};
    std::size_t head_{}, count_{};
    // Time units = master clocks * output_hz. Each output interval is exactly
    // master_hz units, so neither nonintegral rates nor consumer chunks drift.
    std::uint64_t time_{}, produced_{}, clipped_{};
    std::int64_t left_area_{}, right_area_{};
    // Derived only; rebuilt after restore. Avoid per-instruction gain
    // arithmetic and repeated source scans between actual DAC changes.
    std::uint64_t next_event_{~std::uint64_t{0}};
    std::int64_t left_level_{}, right_level_{};
};
} // namespace gameboy
