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
    // Test-only clock fixture supplies output-volume values latched at their
    // respective DSP output phases. Other registers remain whole-sample only.
    [[nodiscard]] std::optional<StereoSample> next_sample_with_output_volumes(
        std::uint8_t master_left, std::uint8_t master_right,
        std::uint8_t echo_volume_left, std::uint8_t echo_volume_right) noexcept;
    // Clock fixture: output at DSP phase 27, clear polled KON at phase 29,
    // then advance voices/keys at phase 30, once per 32-clock sample.
    [[nodiscard]] std::optional<StereoSample> output_timed_sample(
        std::uint8_t master_left, std::uint8_t master_right,
        std::uint8_t echo_volume_left, std::uint8_t echo_volume_right) noexcept;
    void timed_phase29() noexcept { keys_.timed_phase29(); }
    void advance_timed_sample() noexcept;
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

    gameboy::SnesApuBus& bus_;
    std::array<Voice, 8> voices_;
    gameboy::SnesDspRateClock rates_;
    gameboy::SnesDspKeyControl keys_;
    gameboy::SnesDspEndState ends_;
    std::uint8_t timed_endx_pending_{};
    std::uint8_t timed_endx_visible_{};
    bool timed_mode_{};
    [[nodiscard]] StereoSample mix_echo(StereoSample dac_mix,
                                        StereoSample dac_send,
                                        std::uint8_t master_left,
                                        std::uint8_t master_right,
                                        std::uint8_t echo_volume_left,
                                        std::uint8_t echo_volume_right) noexcept;
    void advance_sample(bool timed) noexcept;
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
