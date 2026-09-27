#include "gameboy/snes_brr.hpp"
#include "gameboy/snes_dsp_gaussian.hpp"
#include "gameboy/snes_dsp_voice_math.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

int failures{};

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_coefficient_order_and_rounding() {
    using Gaussian = gameboy::SnesDspGaussian;
    // At d=0, the documented table indices 255,511,256,0 contain
    // 370,1305,374,0; at d=255 the order reverses.
    check(Gaussian::interpolate({1024, 0, 0, 0}, 0) == 185 &&
              Gaussian::interpolate({0, 1024, 0, 0}, 0) == 652 &&
              Gaussian::interpolate({0, 0, 1024, 0}, 0) == 187 &&
              Gaussian::interpolate({0, 0, 0, 1024}, 0) == 0,
          "fraction zero uses the four measured coefficients in the right order");
    check(Gaussian::interpolate({1024, 0, 0, 0}, 255) == 0 &&
              Gaussian::interpolate({0, 1024, 0, 0}, 255) == 187 &&
              Gaussian::interpolate({0, 0, 1024, 0}, 255) == 652 &&
              Gaussian::interpolate({0, 0, 0, 1024}, 255) == 185,
          "fraction 255 mirrors the coefficient selection");
    check(Gaussian::interpolate({1024, 0, 0, 0}, 128) == 28 &&
              Gaussian::interpolate({0, 1024, 0, 0}, 128) == 482 &&
              Gaussian::interpolate({0, 0, 1024, 0}, 128) == 484 &&
              Gaussian::interpolate({0, 0, 0, 1024}, 128) == 29,
          "fraction 128 selects the middle of both coefficient halves");
    check(Gaussian::interpolate({-1024, -1024, 0, 0}, 0) == -838,
          "each negative product rounds down before terms are added");
    check(Gaussian::interpolate({1024, 1024, 1024, 1024}, 0) == 1024 &&
              Gaussian::interpolate({1024, 1024, 1024, 1024}, 255) == 1024,
          "constant samples preserve the symmetric endpoint response");
    check(Gaussian::interpolate({16383, 16383, 16383, 16383}, 0) == -16379 &&
              Gaussian::interpolate({16383, 16383, 16383, 16383}, 255) == 16383,
          "first-three signed-15-bit wrap precedes the last-term clamp");
}

void test_bounds_and_fraction_sweep() {
    using Gaussian = gameboy::SnesDspGaussian;
    for (unsigned fraction = 0; fraction < 256; ++fraction) {
        const auto d = static_cast<std::uint8_t>(fraction);
        const auto high = Gaussian::interpolate({16383, 16383, 16383, 16383}, d);
        const auto low = Gaussian::interpolate({-16384, -16384, -16384, -16384}, d);
        check(high >= -16384 && high <= 16383 &&
                  low >= -16384 && low <= 16383,
              "all fractions stay within the signed 15-bit output range");
    }
    check(Gaussian::interpolate({0, 0, 0, 0}, 73) == 0,
          "silent BRR samples interpolate to silence");
}

void test_brr_to_stereo_boundary() {
    gameboy::SnesBrrDecoder decoder;
    gameboy::SnesBrrDecoder::EncodedBlock block{};
    block[0] = 0x80; // direct filter, shift 8
    block[1] = 0x40; // first sample +512
    const auto decoded = decoder.decode(block);
    std::array<std::int16_t, 4> window{};
    for (unsigned i = 0; i < window.size(); ++i) window[i] = decoded.samples[i];
    const auto interpolated = gameboy::SnesDspGaussian::interpolate(window, 0);
    const auto gained = gameboy::SnesDspVoiceMath::apply_envelope(interpolated, 1024);
    const auto stereo = gameboy::SnesDspVoiceMath::apply_channel_volume(
        gameboy::SnesDspVoiceMath::expand_to_16bit(gained), 0x40);
    check(decoded.samples[0] == 512 && interpolated == 92 &&
              gained == 46 && stereo == 46,
          "decoded BRR can pass through interpolation, envelope, and channel volume");
}

} // namespace

int main() {
    test_coefficient_order_and_rounding();
    test_bounds_and_fraction_sweep();
    test_brr_to_stereo_boundary();
    return failures == 0 ? 0 : 1;
}
