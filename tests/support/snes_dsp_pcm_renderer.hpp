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

// A deliberately test-only 32 kHz PCM path. Register writes must go through
// write_dsp(); the bus does not notify observers about SPC700 DSP writes.
// No SPC700/65C816 scheduling is modeled.
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
    // ENDX is computed during the prior voice step, then exposed at each
    // voice's staggered V7 phase (voice 0 at 2, voice 1 at 5, etc.).
    void publish_timed_endx(unsigned voice) noexcept;
    [[nodiscard]] std::uint8_t endx() const noexcept {
        return timed_mode_ ? timed_endx_visible_ : ends_.endx();
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
    bool timed_mode_{};
    gameboy::SnesDspKeyControl::Sample current_keys_{};
    std::array<std::int16_t, 8> voice_output16_{};
    std::array<TimedVoiceRegisters, 8> timed_voice_registers_{};
    [[nodiscard]] StereoSample mix_echo(StereoSample dac_mix,
                                        StereoSample dac_send,
                                        std::uint8_t master_left,
                                        std::uint8_t master_right,
                                        std::uint8_t echo_volume_left,
                                        std::uint8_t echo_volume_right) noexcept;
    void advance_sample() noexcept;
    void begin_sample(bool timed) noexcept;
    void advance_voice(unsigned index, bool timed) noexcept;
    void mix_voice_channel(unsigned voice, unsigned channel) noexcept;
    StereoSample pending_mix_{};
    StereoSample pending_echo_send_{};
    std::array<StereoSample, 8> echo_history_{};
    std::uint16_t echo_offset_{};
    std::uint16_t echo_length_{};
    std::uint8_t echo_history_position_{};
    std::uint8_t echo_esa_{};
    std::uint16_t noise_ = 0x4000;
};

} // namespace sgb_test
