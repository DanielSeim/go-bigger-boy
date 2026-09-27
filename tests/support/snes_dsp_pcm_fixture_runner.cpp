#include "snes_dsp_fixture.hpp"
#include "snes_dsp_pcm_renderer.hpp"

#include "gameboy/snes_audio_host.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace {

class Sink final {
public:
    Sink() : renderer_(bus_) {}
    void ram(const std::uint16_t address, const std::uint8_t value) {
        if (clock_count_ != 0) supported_ = false;
        bus_.spc_write(address, value);
    }
    void reg(const std::uint8_t address, const std::uint8_t value) {
        if (clock_count_ != 0 && address != 0x0C && address != 0x1C &&
            address != 0x2C && address != 0x3C &&
            address != 0x4C && address != 0x5C) {
            supported_ = false;
        }
        renderer_.write_dsp(address, value);
    }
    [[nodiscard]] std::optional<std::array<std::int16_t, 2>> step() {
        const auto sample = renderer_.next_sample();
        if (!sample) return std::nullopt;
        return std::array<std::int16_t, 2>{sample->left, sample->right};
    }
    [[nodiscard]] std::uint8_t endx() const noexcept { return renderer_.endx(); }
    [[nodiscard]] sgb_test::DspClockResult step_result() {
        if (!supported_ || clock_count_ != 0) return {false, {}};
        stepped_ = true;
        const auto sample = step();
        return {sample.has_value(), sample};
    }
    [[nodiscard]] sgb_test::DspClockResult clock() {
        if (!supported_ || stepped_) return {false, {}};
        const auto phase = clock_count_ % 32;
        // The S-DSP publishes voice n's ENDX bit at phase 2 + 3*n.
        if (phase >= 2 && phase <= 23 && (phase - 2) % 3 == 0) {
            renderer_.publish_timed_endx(static_cast<unsigned>((phase - 2) / 3));
        }
        if (phase == 26) {
            left_volume_ = bus_.dsp_register(0x0C);
            left_echo_volume_ = bus_.dsp_register(0x2C);
        }
        if (phase == 27) {
            right_volume_ = bus_.dsp_register(0x1C);
            right_echo_volume_ = bus_.dsp_register(0x3C);
        }
        ++clock_count_;
        if (phase == 29) renderer_.timed_phase29();
        if (phase == 30) renderer_.advance_timed_sample();
        if (phase != 27) return {true, {}};
        const auto sample = renderer_.output_timed_sample(
            left_volume_, right_volume_, left_echo_volume_, right_echo_volume_);
        if (!sample) return {false, {}};
        return {true, std::array<std::int16_t, 2>{sample->left, sample->right}};
    }

private:
    gameboy::SnesApuBus bus_;
    sgb_test::SnesDspPcmRenderer renderer_;
    std::uint64_t clock_count_{};
    std::uint8_t left_volume_{};
    std::uint8_t right_volume_{};
    std::uint8_t left_echo_volume_{};
    std::uint8_t right_echo_volume_{};
    bool supported_{true};
    bool stepped_{};
};

} // namespace

int main() {
    Sink sink;
    return sgb_test::run_dsp_fixture(sink);
}
