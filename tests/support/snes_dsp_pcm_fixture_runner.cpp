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
        if (clock_count_ != 0 && address != 0x0C && address != 0x1C) {
            supported_ = false;
        }
        renderer_.write_dsp(address, value);
    }
    [[nodiscard]] std::optional<std::array<std::int16_t, 2>> step() {
        const auto sample = renderer_.next_sample();
        if (!sample) return std::nullopt;
        return std::array<std::int16_t, 2>{sample->left, sample->right};
    }
    [[nodiscard]] sgb_test::DspClockResult step_result() {
        if (!supported_ || clock_count_ != 0) return {false, {}};
        stepped_ = true;
        const auto sample = step();
        return {sample.has_value(), sample};
    }
    [[nodiscard]] sgb_test::DspClockResult clock() {
        if (!supported_ || stepped_) return {false, {}};
        const auto phase = clock_count_ % 32;
        if (phase == 26) left_volume_ = bus_.dsp_register(0x0C);
        if (phase == 27) right_volume_ = bus_.dsp_register(0x1C);
        ++clock_count_;
        if (phase != 27) return {true, {}};
        const auto sample = renderer_.next_sample_with_master_volume(
            left_volume_, right_volume_);
        if (!sample) return {false, {}};
        return {true, std::array<std::int16_t, 2>{sample->left, sample->right}};
    }

private:
    gameboy::SnesApuBus bus_;
    sgb_test::SnesDspPcmRenderer renderer_;
    std::uint64_t clock_count_{};
    std::uint8_t left_volume_{};
    std::uint8_t right_volume_{};
    bool supported_{true};
    bool stepped_{};
};

} // namespace

int main() {
    Sink sink;
    return sgb_test::run_dsp_fixture(sink);
}
