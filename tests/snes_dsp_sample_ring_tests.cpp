#include "gameboy/snes_brr.hpp"
#include "gameboy/snes_dsp_sample_ring.hpp"

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

void prefill(gameboy::SnesDspSampleRing& ring) {
    ring.load_group({0, 1, 2, 3});
    ring.load_group({4, 5, 6, 7});
    ring.load_group({8, 9, 10, 11});
}

void test_prefill_and_pitch() {
    gameboy::SnesDspSampleRing ring;
    prefill(ring);
    check(ring.write_position() == 0 && ring.phase() == 0 &&
              ring.window() == gameboy::SnesDspSampleRing::Group{0, 1, 2, 3},
          "three key-on prefill groups wrap the ring write pointer to zero");
    check(ring.predictor_history() == std::array<std::int16_t, 2>{11, 10},
          "BRR predictor reads the two samples before the next physical write slot");
    for (unsigned position = 1; position <= 4; ++position) {
        ring.advance_pitch(0x1000);
        check(ring.phase() == position * 0x1000 &&
                  ring.window() == gameboy::SnesDspSampleRing::Group{
                      static_cast<std::int16_t>(position),
                      static_cast<std::int16_t>(position + 1),
                      static_cast<std::int16_t>(position + 2),
                      static_cast<std::int16_t>(position + 3)},
              "pitch 0x1000 moves the four-point window by one decoded sample");
    }
    check(ring.group_due(),
          "crossing 0x4000 schedules exactly one new BRR group");
    ring.load_group({12, 13, 14, 15});
    check(ring.write_position() == 4,
          "next BRR group overwrites the oldest physical group");
    check(ring.predictor_history() == std::array<std::int16_t, 2>{15, 14},
          "predictor history follows the most recently written physical group");
    ring.advance_pitch(0x1000);
    check(ring.phase() == 0x1000 && !ring.group_due() &&
              ring.window() == gameboy::SnesDspSampleRing::Group{5, 6, 7, 8},
          "after loading a group, the retained fraction selects the next window");
}

void test_ring_wrap_and_key_on_history() {
    gameboy::SnesDspSampleRing ring;
    prefill(ring);
    ring.advance_pitch(0x3FFF);
    ring.advance_pitch(0x3FFF);
    check(ring.phase() == 0x7FFE && ring.group_due() &&
              ring.window() == gameboy::SnesDspSampleRing::Group{7, 8, 9, 10},
          "high pitch can leave a late four-point window before group loading");
    ring.load_group({12, 13, 14, 15});
    ring.advance_pitch(0x3FFF, 16383, true);
    check(ring.phase() == 0x7FFF &&
              ring.window() == gameboy::SnesDspSampleRing::Group{11, 12, 13, 14},
          "modulated pitch clips phase and window wraps across the physical ring");
    ring.key_on();
    check(ring.phase() == 0 && ring.write_position() == 0 &&
              ring.window() == gameboy::SnesDspSampleRing::Group{12, 13, 14, 15},
          "key-on resets ring position without erasing physical sample history");
    check(ring.predictor_history() == std::array<std::int16_t, 2>{11, 10},
          "key-on predictor comes from the physical end, not the latest group");
    ring.reset();
    check(ring.phase() == 0 && ring.write_position() == 0 &&
              ring.window() == gameboy::SnesDspSampleRing::Group{},
          "power-on reset clears the physical ring");
    check(ring.predictor_history() == std::array<std::int16_t, 2>{0, 0},
          "power-on reset clears the physical predictor history");
}

void test_fraction_and_limits() {
    gameboy::SnesDspSampleRing ring;
    ring.load_group({0, 1024, 0, 0});
    ring.load_group({0, 0, 0, 0});
    ring.load_group({0, 0, 0, 0});
    check(ring.interpolated() == 652,
          "ring forwards the four-point window to Gaussian interpolation");
    ring.advance_pitch(0x0800);
    check(ring.phase() == 0x0800 && ring.window() ==
              gameboy::SnesDspSampleRing::Group{0, 1024, 0, 0},
          "sub-sample pitch retains the same integer sample window");
    ring.advance_pitch(0x1800);
    check(ring.phase() == 0x2000 && ring.window() ==
              gameboy::SnesDspSampleRing::Group{0, 0, 0, 0},
          "fractional advances accumulate and cross integer sample boundaries");
    ring.key_on();
    ring.advance_pitch(0xFFFF);
    check(ring.phase() == 0x3FFF,
          "pitch register input is limited to its documented 14 bits");
    ring.advance_pitch(0x3FFF);
    ring.advance_pitch(0x3FFF, 16383, true);
    check(ring.phase() == 0x7FFF,
          "phase saturates at 0x7FFF under positive pitch modulation");
    ring.key_on();
    ring.advance_pitch(0x1000, 1024, true);
    check(ring.phase() == 0x1080,
          "positive previous-voice output raises effective pitch");
    ring.key_on();
    ring.advance_pitch(0x1000, 2048, true);
    check(ring.phase() == 0x1100,
          "modulation uses the full signed 16-bit pre-volume voice output");
    ring.key_on();
    ring.advance_pitch(0x1000, -1024, true);
    check(ring.phase() == 0x0F80,
          "negative previous-voice output lowers effective pitch");
    ring.key_on();
    ring.advance_pitch(0x0400, -33, true);
    check(ring.phase() == 0x03FE,
          "negative modulation uses arithmetic floor shifts, not truncation");
    ring.key_on();
    ring.advance_pitch(0x1000, 1024, false);
    check(ring.phase() == 0x1000,
          "caller can disable modulation for voice zero and noise voices");
}

void test_decoded_brr_groups() {
    gameboy::SnesBrrDecoder decoder;
    gameboy::SnesBrrDecoder::EncodedBlock encoded{};
    encoded[0] = 0x80; // direct filter, shift 8
    encoded[1] = 0x12;
    encoded[2] = 0x34;
    encoded[3] = 0x56;
    encoded[4] = 0x70;
    const auto decoded = decoder.decode(encoded);

    gameboy::SnesDspSampleRing ring;
    for (unsigned group = 0; group < 3; ++group) {
        gameboy::SnesDspSampleRing::Group samples{};
        for (unsigned i = 0; i < samples.size(); ++i) {
            samples[i] = decoded.samples[group * 4 + i];
        }
        ring.load_group(samples);
    }
    check(ring.window() == gameboy::SnesDspSampleRing::Group{128, 256, 384, 512},
          "decoded BRR samples fill the four-point ring window in order");
    for (unsigned i = 0; i < 4; ++i) ring.advance_pitch(0x1000);
    check(ring.group_due() &&
              ring.window() == gameboy::SnesDspSampleRing::Group{640, 768, 896, 0},
          "pitch advances from the first decoded group into the second");
}

} // namespace

int main() {
    test_prefill_and_pitch();
    test_ring_wrap_and_key_on_history();
    test_fraction_and_limits();
    test_decoded_brr_groups();
    return failures == 0 ? 0 : 1;
}
