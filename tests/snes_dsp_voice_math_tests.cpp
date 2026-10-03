#include "gameboy/snes_brr.hpp"
#include "gameboy/snes_dsp_voice_math.hpp"

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

void test_gain_and_release() {
    using Math = gameboy::SnesDspVoiceMath;
    check(Math::direct_gain(0) == 0 && Math::direct_gain(1) == 16 &&
              Math::direct_gain(64) == 1024 && Math::direct_gain(127) == 2032,
          "direct GAIN occupies the upper seven bits of the 11-bit envelope");
    check(Math::release_step(0) == 0 && Math::release_step(7) == 0 &&
              Math::release_step(8) == 0 && Math::release_step(9) == 1 &&
              Math::release_step(0x7FF) == 0x7F7,
          "release subtracts eight per DSP output sample and saturates at zero");
    check(Math::apply_envelope(1000, 1024) == 500 &&
              Math::apply_envelope(-1001, 1024) == -501 &&
              Math::apply_envelope(14336, 2032) == 14224,
          "11-bit envelope multiplication uses signed floor rounding");
    check(Math::apply_envelope(-16384, 0) == 0 &&
              Math::apply_envelope(16383, 0x7FF) == 16375,
          "zero and maximum envelopes stay in the signed 15-bit range");
}

void test_stereo_fixed_point() {
    using Math = gameboy::SnesDspVoiceMath;
    check(Math::expand_to_16bit(-16384) == -32768 &&
              Math::expand_to_16bit(16383) == 32766,
          "DSP restores the missing low bit when expanding 15-bit samples");
    check(Math::apply_channel_volume(1000, 0x7F) == 992 &&
              Math::apply_channel_volume(1000, 0x80) == -1000 &&
              Math::apply_channel_volume(1000, 0xC0) == -500 &&
              Math::apply_channel_volume(-1001, 0x40) == -501,
          "signed left/right volume supports phase inversion and negative rounding");
    check(Math::apply_channel_volume(-32768, 0x80) == 32767 &&
              Math::saturating_add(32000, 1000) == 32767 &&
              Math::saturating_add(-32000, -1000) == -32768,
          "voice volume and mixer addition clamp to signed 16-bit output");
    for (unsigned volume = 0; volume < 256; ++volume)
        check(Math::apply_channel_volume(0, static_cast<std::uint8_t>(volume)) == 0,
              "silent voice is zero at every signed channel volume");
    for (int value = -32768; value <= 32767; ++value)
        check(Math::saturating_add(static_cast<std::int16_t>(value), 0) == value,
              "zero contribution preserves every int16 accumulator exactly");
}

void test_decoded_sample_boundary() {
    gameboy::SnesBrrDecoder decoder;
    gameboy::SnesBrrDecoder::EncodedBlock block{};
    block[0] = 0x80; // shift 8, direct filter
    block[1] = 0x40; // sample 0 is +4 => 512 signed 15-bit
    const auto decoded = decoder.decode(block);
    const auto with_envelope = gameboy::SnesDspVoiceMath::apply_envelope(
        decoded.samples[0], gameboy::SnesDspVoiceMath::direct_gain(64));
    const auto expanded = gameboy::SnesDspVoiceMath::expand_to_16bit(with_envelope);
    check(decoded.samples[0] == 512 && with_envelope == 256 &&
              gameboy::SnesDspVoiceMath::apply_channel_volume(expanded, 0x40) == 256,
          "BRR output feeds deterministic gain and stereo volume math");
}

} // namespace

int main() {
    test_gain_and_release();
    test_stereo_fixed_point();
    test_decoded_sample_boundary();
    return failures == 0 ? 0 : 1;
}
