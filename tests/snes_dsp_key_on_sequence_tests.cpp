#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_brr_group_stream.hpp"
#include "gameboy/snes_dsp_envelope.hpp"
#include "gameboy/snes_dsp_key_on_sequence.hpp"
#include "gameboy/snes_dsp_sample_ring.hpp"
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

using Step = gameboy::SnesDspKeyOnSequence::Step;

bool same(const Step& actual, const Step& expected) {
    return actual.force_silence == expected.force_silence &&
           actual.read_source == expected.read_source &&
           actual.decode_group == expected.decode_group &&
           actual.clock_envelope == expected.clock_envelope &&
           actual.advance_pitch == expected.advance_pitch;
}

void test_startup_actions_and_retrigger() {
    gameboy::SnesDspKeyOnSequence sequence;
    gameboy::SnesDspSampleRing ring;
    gameboy::SnesDspEnvelope envelope;
    ring.load_group({1, 2, 3, 4});
    ring.load_group({5, 6, 7, 8});
    ring.load_group({9, 10, 11, 12});
    ring.advance_pitch(0x1000);
    sequence.key_on(ring, envelope);
    check(ring.phase() == 0 && ring.write_position() == 0 &&
              ring.predictor_history() == std::array<std::int16_t, 2>{12, 11} &&
              envelope.value() == 0 &&
              envelope.phase() == gameboy::SnesDspEnvelope::Phase::attack,
          "accepted KON resets phase and envelope but retains physical ring data");

    check(same(sequence.next(ring), Step{true, true, false, false, false}),
          "silent sample one reads the source pointer only");
    for (unsigned preload = 0; preload < 3; ++preload) {
        check(same(sequence.next(ring), Step{true, false, true, false, false}),
              "silent samples two through four request one BRR group each");
    }
    check(same(sequence.next(ring), Step{true, false, false, true, false}),
          "silent sample five starts envelope updates without advancing pitch");
    check(same(sequence.next(ring), Step{false, false, false, true, true}),
          "sample six permits the first data output and pitch advance");

    sequence.key_on(ring, envelope);
    check(same(sequence.next(ring), Step{true, true, false, false, false}),
          "a second accepted KON restarts the five-sample sequence");
    sequence.reset();
    check(same(sequence.next(ring), Step{false, false, false, true, true}),
          "explicit sequencer reset returns to ordinary per-sample actions");
}

void test_group_due_during_playback() {
    gameboy::SnesDspKeyOnSequence sequence;
    gameboy::SnesDspSampleRing ring;
    gameboy::SnesDspEnvelope envelope;
    sequence.key_on(ring, envelope);
    for (unsigned i = 0; i < 5; ++i) (void)sequence.next(ring);
    for (unsigned sample = 0; sample < 4; ++sample) {
        const auto action = sequence.next(ring);
        check(!action.decode_group && action.advance_pitch,
              "playback does not decode another group before phase crosses 0x4000");
        ring.advance_pitch(0x1000);
    }
    check(ring.group_due() &&
              same(sequence.next(ring), Step{false, false, true, true, true}),
          "steady playback requests a BRR group when phase reaches 0x4000");
}

void test_cycle_latch_preserves_old_voice() {
    gameboy::SnesDspKeyOnSequence sequence;
    gameboy::SnesDspSampleRing ring;
    gameboy::SnesDspEnvelope envelope;
    gameboy::SnesDspRateClock rates;
    ring.advance_pitch(1437);
    envelope.key_on();
    rates.advance();
    envelope.clock(rates, 0, 0, 127);
    const auto value = envelope.value();
    sequence.latch_key_on();
    check(value != 0 && envelope.value() == value && ring.phase() == 1437,
          "cycle-driven KON latch retains the old envelope and pitch position");
    check(same(sequence.next(ring), Step{true, true, false, false, false}),
          "the following voice step requests source startup after a latched KON");
    ring.key_on();
    envelope.key_on();
    check(ring.phase() == 0 && envelope.value() == 0,
          "the cycle-driven caller clears state at the source startup step");
}

void test_startup_pipeline_handoff() {
    gameboy::SnesApuBus bus;
    bus.spc_write(0x2800, 0x00);
    bus.spc_write(0x2801, 0x80); // SRCN=0 starts at $8000
    bus.spc_write(0x8000, 0x80); // direct BRR, shift 8
    bus.spc_write(0x8001, 0x40); // first decoded sample is +512

    gameboy::SnesDspBrrGroupStream stream(bus);
    gameboy::SnesDspSampleRing ring;
    gameboy::SnesDspEnvelope envelope;
    gameboy::SnesDspRateClock rates;
    gameboy::SnesDspKeyOnSequence sequence;
    sequence.key_on(ring, envelope);

    for (unsigned sample = 1; sample <= 5; ++sample) {
        rates.advance();
        const auto action = sequence.next(ring);
        check(action.force_silence, "first five post-KON output samples are silent");
        if (action.read_source) stream.key_on(0x28, 0);
        if (action.decode_group) (void)stream.decode_into_ring(0x28, 0, ring);
        if (action.clock_envelope) envelope.clock(rates, 0x8F, 0, 0);
        check(ring.phase() == 0,
              "pitch stays at zero throughout key-on prefill and silent envelope update");
    }
    check(stream.next_group_index() == 3 && ring.write_position() == 0 &&
              ring.window()[0] == 512 && envelope.value() == 1024,
          "three decoded groups prefill the ring before the first audible sample");

    rates.advance();
    const auto first_data = sequence.next(ring);
    const auto audible = gameboy::SnesDspVoiceMath::apply_envelope(
        ring.interpolated(), envelope.value());
    check(!first_data.force_silence && !first_data.decode_group &&
              first_data.clock_envelope && first_data.advance_pitch &&
              ring.interpolated() == 92 && audible == 46,
          "sixth sample uses pre-update envelope and first BRR/Gaussian output");
    envelope.clock(rates, 0x8F, 0, 0);
    ring.advance_pitch(0x1000);
    check(ring.phase() == 0x1000 && envelope.value() == 0x7FF,
          "first audible sample then advances pitch and clocks the next envelope");
}

} // namespace

int main() {
    test_startup_actions_and_retrigger();
    test_group_due_during_playback();
    test_cycle_latch_preserves_old_voice();
    test_startup_pipeline_handoff();
    return failures == 0 ? 0 : 1;
}
