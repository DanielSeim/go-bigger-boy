#include "snes_dsp_fixture.hpp"
#include "snes_dsp_pcm_renderer.hpp"
#include "snes_dsp_clock.hpp"

#include "gameboy/snes_audio_host.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace {

class Sink final {
public:
    Sink() : renderer_(bus_), clock_(renderer_, bus_) {}
    void ram(const std::uint16_t address, const std::uint8_t value) {
        if (clock_count_ != 0) supported_ = false;
        bus_.spc_write(address, value);
    }
    void reg(const std::uint8_t address, const std::uint8_t value) {
        if (clock_count_ != 0 && address != 0x0C && address != 0x1C &&
            address != 0x0D && address != 0x2C && address != 0x3C &&
            address != 0x2D && address != 0x4C && address != 0x4D &&
            address != 0x3D && address != 0x5C && address != 0x5D &&
            address != 0x6C && address != 0x6D && address != 0x7D &&
            (address & 0x0FU) > 7 && (address & 0x0FU) != 0x0FU) {
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
    [[nodiscard]] std::optional<unsigned> key_clock() const {
        return clock_.key_poll_clock();
    }
    [[nodiscard]] std::optional<std::uint32_t> state(unsigned voice, unsigned field) const {
        return renderer_.diagnostic_state(voice, field);
    }
    [[nodiscard]] sgb_test::DspClockResult step_result() {
        if (!supported_ || clock_count_ != 0) return {false, {}};
        stepped_ = true;
        const auto sample = step();
        return {sample.has_value(), sample};
    }
    [[nodiscard]] sgb_test::DspClockResult clock() {
        if (!supported_ || stepped_) return {false, {}};
        ++clock_count_;
        const bool output = clock_.phase() == 27;
        const auto sample = clock_.clock();
        if (!output) return {true, {}};
        if (!sample) return {false, {}};
        return {true, std::array<std::int16_t, 2>{sample->left, sample->right}};
    }

private:
    gameboy::SnesApuBus bus_;
    sgb_test::SnesDspPcmRenderer renderer_;
    sgb_test::SnesDspClock clock_;
    std::uint64_t clock_count_{};
    bool supported_{true};
    bool stepped_{};
};

} // namespace

int main() {
    Sink sink;
    return sgb_test::run_dsp_fixture(sink);
}
