#pragma once

#include "gameboy/snes_dsp_brr_group_stream.hpp"
#include "gameboy/snes_dsp_end_state.hpp"
#include "gameboy/snes_dsp_envelope.hpp"
#include "gameboy/snes_dsp_key_control.hpp"
#include "gameboy/snes_dsp_key_on_sequence.hpp"
#include "gameboy/snes_dsp_sample_ring.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace gameboy {
class SnesApuBus;
}

namespace sgb_test {

// A deliberately test-only 32 kHz PCM path. Standalone fixtures use write_dsp();
// shared-bus callers commit an SPC write then feed accept_dsp_write() once.
// Scheduling is supplied by the caller, not by this renderer.
class SnesDspPcmRenderer final {
public:
    struct StereoSample {
        std::int16_t left{};
        std::int16_t right{};
    };
    static constexpr unsigned sample_rate = 32000;

    explicit SnesDspPcmRenderer(gameboy::SnesApuBus& bus) noexcept;
    void reset() noexcept;
    void write_dsp(std::uint8_t address, std::uint8_t value) noexcept;
    // A shared-bus caller has already committed this SPC write. Only update
    // synthesis side effects; do not write the SPC ports again.
    void accept_dsp_write(std::uint8_t address, std::uint8_t value) noexcept;
    void set_live_readback_enabled(bool enabled) noexcept { live_readback_ = enabled; }
    void publish_timed_readback(unsigned phase) noexcept;
    [[nodiscard]] std::optional<StereoSample> next_sample() noexcept;
    // Whole-sample fixture can supply the four output volumes explicitly;
    // clock fixtures use the phase methods below for per-voice reads.
    [[nodiscard]] std::optional<StereoSample> next_sample_with_output_volumes(
        std::uint8_t master_left, std::uint8_t master_right,
        std::uint8_t echo_volume_left, std::uint8_t echo_volume_right) noexcept;
    // Clock fixture: output at phase 27; phase 29 clears polled KON; phase 30
    // polls keys and advances voice 0. Voices 1-7 advance at phases 1,4,...19.
    [[nodiscard]] std::optional<StereoSample> output_timed_sample(
        std::uint8_t master_left, std::uint8_t master_right,
        std::uint8_t echo_volume_left, std::uint8_t echo_volume_right) noexcept;
    void timed_phase29() noexcept { keys_.timed_phase29(); }
    void advance_timed_sample() noexcept;
    void latch_timed_voice_registers(unsigned phase) noexcept;
    void advance_timed_voice(unsigned voice) noexcept;
    void mix_timed_voice_channel(unsigned voice, unsigned channel) noexcept;
    void latch_timed_fir(unsigned phase) noexcept;
    void timed_phase26() noexcept;
    void timed_phase27() noexcept;
    void timed_phase28() noexcept;
    void timed_echo_phase29() noexcept;
    void timed_echo_phase30() noexcept;
    // ENDX is computed during the prior voice step, then exposed at each
    // voice's staggered V7 phase (voice 0 at 2, voice 1 at 5, etc.).
    void publish_timed_endx(unsigned voice) noexcept;
    [[nodiscard]] std::uint8_t endx() const noexcept {
        return timed_mode_ ? timed_endx_visible_ : ends_.endx();
    }
    // Development-only state probe; these are renderer internals, not SNES
    // register reads. Used to localize a divergence in a title capture.
    [[nodiscard]] std::uint32_t diagnostic_state(unsigned voice,
                                                  unsigned field) const noexcept {
        if (voice < voices_.size()) {
            const auto& state = voices_[voice];
            if (field == 0) return state.envelope.value();
            if (field == 1) return state.stream.next_address();
            if (field == 2) return state.ring.phase();
        }
        if (voice == voices_.size() && field == 0) return echo_offset_;
        return 0;
    }

private:
    struct Voice {
        explicit Voice(const gameboy::SnesApuBus& bus) noexcept : stream(bus) {}
        gameboy::SnesDspBrrGroupStream stream;
        gameboy::SnesDspSampleRing ring;
        gameboy::SnesDspEnvelope envelope;
        gameboy::SnesDspKeyOnSequence sequence;
        bool started{};
    };
    struct TimedVoiceRegisters {
        std::uint8_t directory{};
        std::uint8_t source{};
        std::uint8_t pitch_low{};
        std::uint8_t pitch_high{};
        std::uint8_t adsr0{};
    };

    gameboy::SnesApuBus& bus_;
    std::array<Voice, 8> voices_;
    gameboy::SnesDspRateClock rates_;
    gameboy::SnesDspKeyControl keys_;
    gameboy::SnesDspEndState ends_;
    std::uint8_t timed_endx_visible_{};
    std::uint8_t timed_pmon_{};
    std::uint8_t timed_non_{};
    std::uint8_t timed_eon_{};
    std::uint8_t timed_dir_{};
    std::uint8_t timed_feedback_{};
    std::array<std::uint8_t, 8> timed_fir_{};
    std::uint8_t timed_echo_enabled_{};
    bool timed_mode_{};
    gameboy::SnesDspKeyControl::Sample current_keys_{};
    std::array<std::int16_t, 8> voice_output16_{};
    bool live_readback_{};
    std::array<std::uint8_t, 8> live_envx_{};
    std::array<bool, 8> live_loop_event_{};
    std::array<bool, 8> live_kon_event_{};
    std::uint8_t live_envx_buffer_{};
    std::uint8_t live_outx_buffer_{};
    std::uint8_t live_endx_buffer_{};
    StereoSample live_echo_input_{};
    std::array<TimedVoiceRegisters, 8> timed_voice_registers_{};
    [[nodiscard]] StereoSample mix_echo(StereoSample dac_mix,
                                        StereoSample dac_send,
                                        std::uint8_t master_left,
                                        std::uint8_t master_right,
                                        std::uint8_t echo_volume_left,
                                        std::uint8_t echo_volume_right,
                                        bool timed) noexcept;
    void write_echo_channel(std::uint16_t address, unsigned channel,
                            std::int16_t value) noexcept;
    void advance_echo_address() noexcept;
    void advance_sample() noexcept;
    void begin_sample(bool timed) noexcept;
    void advance_voice(unsigned index, bool timed) noexcept;
    void mix_voice_channel(unsigned voice, unsigned channel) noexcept;
    StereoSample pending_mix_{};
    StereoSample pending_echo_send_{};
    StereoSample timed_echo_writeback_{};
    std::uint16_t timed_echo_address_{};
    bool timed_echo_write_pending_{};
    std::array<StereoSample, 8> echo_history_{};
    std::uint16_t echo_offset_{};
    std::uint16_t echo_length_{};
    std::uint8_t echo_history_position_{};
    std::uint8_t echo_esa_{};
    std::uint16_t noise_ = 0x4000;
};

} // namespace sgb_test
