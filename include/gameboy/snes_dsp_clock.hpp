#pragma once

#include "gameboy/snes_dsp_pcm_renderer.hpp"
#include "gameboy/snes_audio_host.hpp"

namespace gameboy {

// DSP phase driver shared by core buffering and diagnostic callers.
// clock() executes one DSP phase; output is sampled immediately after phase 27.
class SnesDspClock final {
public:
    SnesDspClock(SnesDspPcmRenderer& renderer, gameboy::SnesApuBus& bus)
        : renderer_(renderer), bus_(bus) {}
    void reset() noexcept { count_ = 0; left_ = right_ = echo_left_ = echo_right_ = 0; }
    [[nodiscard]] unsigned phase() const noexcept { return count_ % 32; }
    [[nodiscard]] unsigned key_poll_clock() const noexcept { return count_ % 64; }
    [[nodiscard]] std::optional<SnesDspPcmRenderer::StereoSample> clock() noexcept {
        const auto p = phase();
        renderer_.latch_timed_voice_registers(p);
        if (p == 0) renderer_.mix_timed_voice_channel(0, 1);
        if (p == 31) renderer_.mix_timed_voice_channel(0, 0);
        if (p >= 1 && p <= 19 && (p - 1) % 3 == 0)
            renderer_.advance_timed_voice((p + 2) / 3);
        if (p >= 2 && p <= 20 && (p - 2) % 3 == 0)
            renderer_.mix_timed_voice_channel((p + 1) / 3, 0);
        if (p >= 3 && p <= 21 && (p - 3) % 3 == 0)
            renderer_.mix_timed_voice_channel(p / 3, 1);
        if (p >= 2 && p <= 23 && (p - 2) % 3 == 0)
            renderer_.publish_timed_endx((p - 2) / 3);
        renderer_.publish_timed_readback(p);
        if (p >= 22 && p <= 25) renderer_.latch_timed_fir(p);
        if (p == 26) {
            left_ = bus_.dsp_register(0x0c);
            echo_left_ = bus_.dsp_register(0x2c);
            renderer_.timed_phase26();
        }
        if (p == 27) {
            right_ = bus_.dsp_register(0x1c);
            echo_right_ = bus_.dsp_register(0x3c);
            renderer_.timed_phase27();
        }
        if (p == 28) renderer_.timed_phase28();
        if (p == 29) {
            renderer_.timed_phase29();
            renderer_.timed_echo_phase29();
        }
        if (p == 30) {
            renderer_.advance_timed_sample();
            renderer_.timed_echo_phase30();
        }
        ++count_;
        if (p != 27) return std::nullopt;
        return renderer_.output_timed_sample(left_, right_, echo_left_, echo_right_);
    }
private:
    friend class SnesDspStateCodec;
    SnesDspPcmRenderer& renderer_;
    gameboy::SnesApuBus& bus_;
    std::uint64_t count_{};
    std::uint8_t left_{}, right_{}, echo_left_{}, echo_right_{};
};

} // namespace gameboy
