#include "gameboy/snes_brr.hpp"

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

void test_nibble_order_and_shift() {
    gameboy::SnesBrrDecoder decoder;
    gameboy::SnesBrrDecoder::EncodedBlock block{};
    block[0] = 0x10; // shift 1, direct filter
    block[1] = 0x17;
    block[2] = 0x8F;
    const auto result = decoder.decode(block);
    check(result.samples[0] == 1 && result.samples[1] == 7 &&
              result.samples[2] == -8 && result.samples[3] == -1,
          "BRR reads high nibble first and sign-extends each four-bit sample");
    check(!result.end && !result.loop,
          "BRR end and loop flags are independent of decoded samples");

    decoder.reset();
    block.fill(0);
    block[0] = 0x00;
    block[1] = 0x1F;
    const auto low_shift = decoder.decode(block);
    check(low_shift.samples[0] == 0 && low_shift.samples[1] == -1,
          "BRR shift zero discards a low bit with signed rounding");

    decoder.reset();
    block.fill(0);
    block[0] = 0xC1;
    block[1] = 0x78;
    const auto largest_normal = decoder.decode(block);
    check(largest_normal.end && !largest_normal.loop &&
              largest_normal.samples[0] == 14336 &&
              largest_normal.samples[1] == -16384,
          "shift 12 reaches the extremes of the signed 15-bit sample range");

    for (std::uint8_t shift = 13; shift <= 15; ++shift) {
        decoder.reset();
        block.fill(0);
        block[0] = static_cast<std::uint8_t>(
            (static_cast<unsigned>(shift) << 4) | 3U);
        block[1] = 0x7F;
        const auto unusual = decoder.decode(block);
        check(unusual.end && unusual.loop && unusual.samples[0] == 0 &&
                  unusual.samples[1] == -2048,
              "BRR shifts 13 through 15 use sign-only residuals");
    }
}

void test_filter_history_and_overflow() {
    gameboy::SnesBrrDecoder::EncodedBlock seed{};
    seed[0] = 0x10; // shift 1, direct filter
    seed[8] = 0x87; // last two decoded samples: -8, +7
    gameboy::SnesBrrDecoder::EncodedBlock filtered{};

    gameboy::SnesBrrDecoder filter1;
    seed[8] = 0x17; // last two decoded samples: +1, +7
    (void)filter1.decode(seed);
    filtered[0] = 0x14; // shift 1, filter 1
    const auto one = filter1.decode(filtered);
    check(one.samples[0] == 6 && one.samples[1] == 5,
          "filter 1 carries history across block boundaries with floor rounding");

    seed[8] = 0x87;
    gameboy::SnesBrrDecoder filter2;
    (void)filter2.decode(seed);
    filtered[0] = 0x18; // shift 1, filter 2
    const auto two = filter2.decode(filtered);
    check(two.samples[0] == 20 && two.samples[1] == 31,
          "filter 2 uses the two previous samples and signed shifts");
    filter2.reset();
    check(filter2.decode(filtered).samples[0] == 0,
          "reset drops BRR prediction history");

    gameboy::SnesBrrDecoder filter3;
    (void)filter3.decode(seed);
    filtered[0] = 0x1C; // shift 1, filter 3
    const auto three = filter3.decode(filtered);
    check(three.samples[0] == 18 && three.samples[1] == 26,
          "filter 3 has distinct fixed-point prediction from filter 2");

    gameboy::SnesBrrDecoder overflow;
    seed[0] = 0xC0;
    (void)overflow.decode(seed);
    filtered[0] = 0x08;
    check(overflow.decode(filtered).samples[0] == -1,
          "BRR prediction clamps to 16 bits then wraps to signed 15 bits");
}

} // namespace

int main() {
    test_nibble_order_and_shift();
    test_filter_history_and_overflow();
    return failures == 0 ? 0 : 1;
}
