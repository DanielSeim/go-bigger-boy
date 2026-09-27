#include "snes_dsp_fixture.hpp"

#include "SPC_DSP.h" // supplied by an external, development-only checkout

#include <array>
#include <cstdint>
#include <optional>

namespace {

class Sink final {
public:
    Sink() { dsp_.init(ram_.data(), ram_.data()); }
    void ram(const std::uint16_t address, const std::uint8_t value) {
        ram_[address] = value;
    }
    void reg(const std::uint8_t address, const std::uint8_t value) {
        dsp_.write(address, value);
    }
    [[nodiscard]] std::optional<std::array<std::int16_t, 2>> step() {
        // Leave one stereo slot spare: this implementation redirects its
        // output pointer to an internal buffer when a buffer fills exactly.
        std::array<SPC_DSP::sample_t, 4> output{};
        dsp_.set_output(output.data(), 4);
        dsp_.run(32);
        if (dsp_.sample_count() != 2) return std::nullopt;
        return std::array<std::int16_t, 2>{output[0], output[1]};
    }
    [[nodiscard]] sgb_test::DspClockResult step_result() {
        const auto sample = step();
        return {sample.has_value(), sample};
    }
    [[nodiscard]] sgb_test::DspClockResult clock() {
        std::array<SPC_DSP::sample_t, 4> output{};
        dsp_.set_output(output.data(), 4);
        dsp_.run(1);
        if (dsp_.sample_count() == 0) return {true, {}};
        if (dsp_.sample_count() != 2) return {false, {}};
        return {true, std::array<std::int16_t, 2>{output[0], output[1]}};
    }

private:
    std::array<std::uint8_t, 0x10000> ram_{};
    SPC_DSP dsp_{};
};

} // namespace

int main() {
    Sink sink;
    return sgb_test::run_dsp_fixture(sink);
}
