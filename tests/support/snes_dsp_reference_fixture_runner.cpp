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
    void spc(std::uint16_t address, std::uint8_t value) {
        ram_[address] = value;
        if (address == 0xf2) selector_ = value;
        if (address == 0xf3 && selector_ < 128) dsp_.write(selector_, value);
    }
    [[nodiscard]] std::optional<std::uint8_t> readreg(unsigned address) const {
        return dsp_.read(address);
    }
    [[nodiscard]] std::optional<std::uint8_t> readram(unsigned address) const {
        return ram_[address];
    }
    [[nodiscard]] std::optional<std::uint8_t> spcread(unsigned address) const {
        if (address == 0xf2) return selector_;
        if (address == 0xf3) return dsp_.read(selector_ & 127);
        // Shared fixtures do not enable timers or host input ports.
        if (address >= 0xf0 && address <= 0xff) return 0;
        return ram_[address];
    }
    [[nodiscard]] std::uint8_t endx() const { return dsp_.read(0x7c); }
    [[nodiscard]] std::optional<unsigned> key_clock() const {
#ifdef GBB_REFERENCE_KEY_CLOCK
        return dsp_.diagnostic_key_poll_clock();
#else
        return std::nullopt;
#endif
    }
    [[nodiscard]] std::optional<std::uint32_t> state(unsigned voice, unsigned field) const {
#ifdef GBB_REFERENCE_DIAGNOSTIC_STATE
        return static_cast<std::uint32_t>(dsp_.diagnostic_state(voice, field));
#else
        (void)voice;
        (void)field;
        return std::nullopt;
#endif
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
    std::uint8_t selector_{};
};

} // namespace

int main() {
    Sink sink;
    return sgb_test::run_dsp_fixture(sink);
}
