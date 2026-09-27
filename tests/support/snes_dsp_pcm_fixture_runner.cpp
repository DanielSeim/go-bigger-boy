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
        bus_.spc_write(address, value);
    }
    void reg(const std::uint8_t address, const std::uint8_t value) {
        renderer_.write_dsp(address, value);
    }
    [[nodiscard]] std::optional<std::array<std::int16_t, 2>> step() {
        const auto sample = renderer_.next_sample();
        if (!sample) return std::nullopt;
        return std::array<std::int16_t, 2>{sample->left, sample->right};
    }

private:
    gameboy::SnesApuBus bus_;
    sgb_test::SnesDspPcmRenderer renderer_;
};

} // namespace

int main() {
    Sink sink;
    return sgb_test::run_dsp_fixture(sink);
}
