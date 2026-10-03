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
        // All phases retain the same ordered operations below. Constant
        // phase arguments remove repeated schedule tests/address arithmetic.
        switch (phase()) {
        case 0: return clock_phase<0>(); case 1: return clock_phase<1>();
        case 2: return clock_phase<2>(); case 3: return clock_phase<3>();
        case 4: return clock_phase<4>(); case 5: return clock_phase<5>();
        case 6: return clock_phase<6>(); case 7: return clock_phase<7>();
        case 8: return clock_phase<8>(); case 9: return clock_phase<9>();
        case 10: return clock_phase<10>(); case 11: return clock_phase<11>();
        case 12: return clock_phase<12>(); case 13: return clock_phase<13>();
        case 14: return clock_phase<14>(); case 15: return clock_phase<15>();
        case 16: return clock_phase<16>(); case 17: return clock_phase<17>();
        case 18: return clock_phase<18>(); case 19: return clock_phase<19>();
        case 20: return clock_phase<20>(); case 21: return clock_phase<21>();
        case 22: return clock_phase<22>(); case 23: return clock_phase<23>();
        case 24: return clock_phase<24>(); case 25: return clock_phase<25>();
        case 26: return clock_phase<26>(); case 27: return clock_phase<27>();
        case 28: return clock_phase<28>(); case 29: return clock_phase<29>();
        case 30: return clock_phase<30>(); default: return clock_phase<31>();
        }
    }
    // Original runtime-phase dispatcher retained as a timing/PCM oracle.
    [[nodiscard]] std::optional<SnesDspPcmRenderer::StereoSample> clock_scalar() noexcept {
        return clock_phase<32>();
    }
private:
    template<unsigned Phase>
    [[nodiscard]] std::optional<SnesDspPcmRenderer::StereoSample> clock_phase() noexcept {
        const auto p = Phase == 32 ? phase() : Phase;
        if (p <= 22 || p == 31) renderer_.latch_timed_voice_registers(p);
        // These three schedules are mutually exclusive. Dispatch once, and
        // retain V7 after the left mix on coincident phases (including V0).
        if (p <= 21) {
            switch (p % 3) {
            case 0: renderer_.mix_timed_voice_channel(p / 3, 1); break;
            case 1: renderer_.advance_timed_voice((p + 2) / 3); break;
            case 2:
                renderer_.mix_timed_voice_channel((p + 1) / 3, 0);
                renderer_.publish_timed_endx((p - 2) / 3);
                break;
            }
        } else if (p == 23) renderer_.publish_timed_endx(7);
        else if (p == 31) renderer_.mix_timed_voice_channel(0, 0);
        if constexpr (Phase <= 25) renderer_.publish_timed_readback_phase<Phase>();
        else if constexpr (Phase == 32) {
            if (p <= 25) renderer_.publish_timed_readback(p);
        }
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
    friend class SnesDspStateCodec;
    SnesDspPcmRenderer& renderer_;
    gameboy::SnesApuBus& bus_;
    std::uint64_t count_{};
    std::uint8_t left_{}, right_{}, echo_left_{}, echo_right_{};
};

} // namespace gameboy
