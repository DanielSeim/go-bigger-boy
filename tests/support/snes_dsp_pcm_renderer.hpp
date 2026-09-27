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
    // Test-only clock fixture supplies the two master-volume values latched
    // at their respective DSP output phases. Other registers remain sampled
    // at the ordinary whole-sample boundary.
    [[nodiscard]] std::optional<StereoSample> next_sample_with_master_volume(
        std::uint8_t left, std::uint8_t right) noexcept;
    [[nodiscard]] std::uint8_t endx() const noexcept { return ends_.endx(); }

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
    [[nodiscard]] StereoSample mix_echo(StereoSample dac_mix,
                                        StereoSample dac_send,
                                        std::uint8_t master_left,
                                        std::uint8_t master_right) noexcept;
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
