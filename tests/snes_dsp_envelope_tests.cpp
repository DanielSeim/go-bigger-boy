#include "gameboy/snes_dsp_envelope.hpp"

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

void tick(gameboy::SnesDspRateClock& rates,
          gameboy::SnesDspEnvelope& envelope,
          const std::uint8_t adsr1,
          const std::uint8_t adsr2,
          const std::uint8_t gain) {
    rates.advance();
    envelope.clock(rates, adsr1, adsr2, gain);
}

void test_global_rate_phase() {
    gameboy::SnesDspRateClock rates;
    constexpr std::array<unsigned, 32> periods{
        0, 2048, 1536, 1280, 1024, 768, 640, 512,
        384, 320, 256, 192, 160, 128, 96, 80,
        64, 48, 40, 32, 24, 20, 16, 12,
        10, 8, 6, 5, 4, 3, 2, 1,
    };
    std::array<unsigned, 32> counts{};
    std::array<unsigned, 32> first{};
    for (unsigned sample = 1; sample <= 30720; ++sample) {
        rates.advance();
        for (unsigned rate = 0; rate < counts.size(); ++rate) {
            if (rates.event(static_cast<std::uint8_t>(rate))) {
                ++counts[rate];
                if (first[rate] == 0) first[rate] = sample;
            }
        }
    }
    check(rates.counter() == 0 && counts[0] == 0,
          "DSP global counter spans 30720 samples and rate zero never ticks");
    for (unsigned rate = 1; rate < counts.size(); ++rate) {
        check(counts[rate] == 30720 / periods[rate],
              "each DSP rate fires its documented number of times per full counter cycle");
    }
    check(first[1] == 2048 && first[2] == 1040 && first[3] == 536 &&
              first[31] == 1,
          "counter offsets preserve the first-event phase for slow rates");
    rates.reset();
    check(rates.counter() == 0,
          "DSP global rate counter resets to zero, not its wrap value");
}

void test_attack_decay_sustain() {
    using Phase = gameboy::SnesDspEnvelope::Phase;
    gameboy::SnesDspRateClock rates;
    gameboy::SnesDspEnvelope envelope;
    envelope.key_on();
    tick(rates, envelope, 0x8F, 0xC0, 0);
    check(envelope.value() == 1024 && envelope.phase() == Phase::attack,
          "fast attack raises the envelope by 1024 at rate 31");
    tick(rates, envelope, 0x8F, 0xC0, 0);
    check(envelope.value() == 0x7FF && envelope.phase() == Phase::decay,
          "attack overflow clamps to 11 bits and enters decay");
    unsigned clocks = 0;
    while (envelope.value() == 0x7FF && clocks < 1000) {
        tick(rates, envelope, 0x8F, 0xC0, 0);
        ++clocks;
    }
    check(envelope.value() == 0x7F7 && envelope.phase() == Phase::decay &&
              clocks > 1,
          "decay uses its slower global rate and exponential step");
    while (envelope.phase() == Phase::decay && clocks < 10000) {
        tick(rates, envelope, 0x8F, 0xC0, 0);
        ++clocks;
    }
    check(envelope.phase() == Phase::sustain &&
              (envelope.value() >> 8) == 6,
          "decay changes to sustain when envelope upper bits reach the level");
    const auto held = envelope.value();
    for (unsigned sample = 0; sample < 100; ++sample) {
        tick(rates, envelope, 0x8F, 0xC0, 0);
    }
    check(envelope.value() == held,
          "sustain rate zero freezes the envelope");

    rates.reset();
    envelope.key_on();
    for (unsigned sample = 0; sample < 2047; ++sample) {
        tick(rates, envelope, 0x80, 0, 0);
    }
    check(envelope.value() == 0,
          "normal attack rate one does not update before its first event");
    tick(rates, envelope, 0x80, 0, 0);
    check(envelope.value() == 32 && envelope.phase() == Phase::attack,
          "normal attack increments 32 at the scheduled rate event");
}

void test_gain_and_release() {
    using Phase = gameboy::SnesDspEnvelope::Phase;
    gameboy::SnesDspRateClock rates;
    gameboy::SnesDspEnvelope envelope;
    envelope.key_on();
    tick(rates, envelope, 0, 0, 0x40);
    check(envelope.value() == 1024,
          "direct gain writes all seven GAIN bits into the 11-bit envelope");
    tick(rates, envelope, 0, 0, 0x9F);
    check(envelope.value() == 992,
          "linear decrease subtracts 32 at rate 31");
    tick(rates, envelope, 0, 0, 0xBF);
    check(envelope.value() == 988,
          "exponential decrease subtracts the rounded 1/256 step");
    tick(rates, envelope, 0, 0, 0xDF);
    check(envelope.value() == 1020,
          "linear increase adds 32 at rate 31");
    tick(rates, envelope, 0, 0, 0xFF);
    check(envelope.value() == 1052,
          "bent increase uses 32 below the 0x600 threshold");
    tick(rates, envelope, 0, 0, 0x60);
    tick(rates, envelope, 0, 0, 0xFF);
    check(envelope.value() == 1544,
          "bent increase uses 8 at or above the 0x600 threshold");
    tick(rates, envelope, 0, 0, 0x80);
    check(envelope.value() == 1544,
          "gain rate zero leaves the envelope unchanged");

    envelope.key_on();
    tick(rates, envelope, 0, 0, 0x7F);
    tick(rates, envelope, 0, 0, 0xDF);
    check(envelope.value() == 0x7FF && envelope.phase() == Phase::decay,
          "ADSR phase latch still sees overflow while GAIN mode supplies the update");

    envelope.key_on();
    tick(rates, envelope, 0, 0, 1);
    envelope.key_off();
    tick(rates, envelope, 0x8F, 0, 0x7F);
    check(envelope.value() == 8 && envelope.phase() == Phase::release,
          "key-off release overrides both ADSR and direct gain");
    tick(rates, envelope, 0x8F, 0, 0x7F);
    check(envelope.value() == 0,
          "release subtracts eight each output sample until zero");
    for (unsigned sample = 0; sample < 16; ++sample) {
        tick(rates, envelope, 0, 0, 0x7F);
    }
    check(envelope.value() == 0 && envelope.phase() == Phase::release,
          "release stays silent despite a nonzero direct gain setting");
    envelope.reset();
    check(envelope.value() == 0 && envelope.phase() == Phase::release,
          "reset returns the envelope to silent release state");
}

void test_gain_clamp_and_rate_switch() {
    using Phase = gameboy::SnesDspEnvelope::Phase;
    gameboy::SnesDspRateClock rates;
    gameboy::SnesDspEnvelope envelope;
    envelope.key_on();
    tick(rates, envelope, 0, 0, 0x7F);
    check(envelope.value() == 2032 && envelope.phase() == Phase::attack,
          "direct gain below the maximum does not advance attack phase");
    tick(rates, envelope, 0, 0, 0xDF);
    check(envelope.value() == 0x7FF && envelope.phase() == Phase::decay,
          "gain increment clamps at maximum before attack transitions to decay");

    envelope.key_on();
    tick(rates, envelope, 0, 0, 0x01);
    tick(rates, envelope, 0, 0, 0x9F);
    check(envelope.value() == 0 && envelope.phase() == Phase::decay,
          "negative gain pre-clamp transitions attack phase even though value clamps to zero");

    envelope.key_on();
    tick(rates, envelope, 0, 0, 0x20);
    for (unsigned sample = 0; sample < 3; ++sample) {
        tick(rates, envelope, 0, 0, 0x80);
    }
    check(envelope.value() == 512,
          "gain rate zero holds current value despite mode changes");
    tick(rates, envelope, 0, 0, 0x9F);
    check(envelope.value() == 480,
          "switching to rate 31 uses the global clock without a per-voice restart");
}

} // namespace

int main() {
    test_global_rate_phase();
    test_attack_decay_sustain();
    test_gain_and_release();
    test_gain_clamp_and_rate_switch();
    return failures == 0 ? 0 : 1;
}
