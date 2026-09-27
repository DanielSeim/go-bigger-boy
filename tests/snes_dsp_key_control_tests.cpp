#include "gameboy/snes_dsp_envelope.hpp"
#include "gameboy/snes_dsp_key_control.hpp"
#include "gameboy/snes_dsp_key_on_sequence.hpp"
#include "gameboy/snes_dsp_sample_ring.hpp"

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

void test_polling_and_register_writes() {
    gameboy::SnesDspKeyControl control;
    control.reset(false);
    control.write_kon(0x01);
    control.write_koff(0x02);
    const auto first = control.next_sample();
    check(first.key_on == 0 && first.key_off == 0,
          "the unselected phase does not sample KON or KOFF");
    control.write_kon(0x04);
    control.write_koff(0x08);
    const auto polled = control.next_sample();
    check(polled.key_on == 0x04 && polled.key_off == 0x08,
          "latest writes replace pending KON and KOFF at the next poll");
    control.write_koff(0);
    const auto between = control.next_sample();
    check(between.key_on == 0 && between.key_off == 0x08,
          "KON is a pulse while sampled KOFF remains active between polls");
    const auto next_poll = control.next_sample();
    check(next_poll.key_on == 0 && next_poll.key_off == 0,
          "KOFF clears at a poll and KON does not retrigger without a write");
    control.write_soft_reset(true);
    check(control.next_sample().soft_reset && control.next_sample().soft_reset,
          "FLG soft reset is observed on both sample phases");
    control.reset();
    check(control.next_sample().key_off == 0 && !control.next_sample().soft_reset,
          "reset clears pending and persistent control state");
}

void test_voice_precedence_and_retrigger() {
    gameboy::SnesDspKeyControl control;
    gameboy::SnesDspSampleRing ring;
    gameboy::SnesDspEnvelope envelope;
    gameboy::SnesDspKeyOnSequence sequence;
    gameboy::SnesDspRateClock rates;
    envelope.key_on();
    rates.advance();
    envelope.clock(rates, 0, 0, 0x7f); // direct gain: nonzero
    ring.load_group({1, 2, 3, 4});
    check(envelope.value() != 0, "fixture starts with an active envelope");

    control.write_koff(0x01);
    auto sample = control.next_sample();
    gameboy::SnesDspKeyControl::apply_voice(0, sample, ring, envelope, sequence);
    const auto before_release = envelope.value();
    check(envelope.phase() == gameboy::SnesDspEnvelope::Phase::release &&
              before_release != 0,
          "KOFF enters release without immediately zeroing the envelope");
    envelope.clock(rates, 0, 0, 0x7f);
    check(envelope.value() == before_release - 8,
          "KOFF follows the normal eight-unit release step");

    control.write_kon(0x01);
    control.write_koff(0x01);
    (void)control.next_sample(); // unpolled phase
    sample = control.next_sample();
    gameboy::SnesDspKeyControl::apply_voice(0, sample, ring, envelope, sequence);
    check(envelope.phase() == gameboy::SnesDspEnvelope::Phase::attack &&
              envelope.value() == 0 && ring.write_position() == 0 &&
              sequence.next(ring).read_source,
          "KON wins over KOFF on the same poll and restarts the source");
    sample = control.next_sample();
    gameboy::SnesDspKeyControl::apply_voice(0, sample, ring, envelope, sequence);
    check(envelope.phase() == gameboy::SnesDspEnvelope::Phase::release &&
              sequence.next(ring).decode_group,
          "persistent KOFF releases the envelope without stopping BRR prefill");

    control.write_koff(0);
    (void)control.next_sample(); // poll KOFF=0
    control.write_kon(0x01);
    sample = control.next_sample(); // no poll
    gameboy::SnesDspKeyControl::apply_voice(0, sample, ring, envelope, sequence);
    check(envelope.phase() == gameboy::SnesDspEnvelope::Phase::release,
          "KON written between polls has not yet retriggered");
    sample = control.next_sample(); // poll KON
    gameboy::SnesDspKeyControl::apply_voice(0, sample, ring, envelope, sequence);
    check(envelope.phase() == gameboy::SnesDspEnvelope::Phase::attack &&
              sequence.next(ring).read_source,
          "a later accepted KON retriggers from the first silent sample");
}

void test_soft_reset_all_voices() {
    gameboy::SnesDspKeyControl control;
    gameboy::SnesDspSampleRing ring;
    gameboy::SnesDspEnvelope envelope;
    gameboy::SnesDspKeyOnSequence sequence;
    gameboy::SnesDspRateClock rates;
    envelope.key_on();
    rates.advance();
    envelope.clock(rates, 0, 0, 0x7f);
    control.write_soft_reset(true);
    auto sample = control.next_sample();
    gameboy::SnesDspKeyControl::apply_voice(7, sample, ring, envelope, sequence);
    check(envelope.value() == 0 &&
              envelope.phase() == gameboy::SnesDspEnvelope::Phase::release,
          "soft reset immediately zeroes the envelope on a high-index voice");
    control.write_kon(0x80);
    (void)control.next_sample(); // unpolled phase
    sample = control.next_sample(); // KON + FLG
    gameboy::SnesDspKeyControl::apply_voice(7, sample, ring, envelope, sequence);
    check(envelope.phase() == gameboy::SnesDspEnvelope::Phase::attack &&
              sequence.next(ring).force_silence,
          "KON takes ordering priority over FLG on the accepted sample");
    sample = control.next_sample();
    gameboy::SnesDspKeyControl::apply_voice(7, sample, ring, envelope, sequence);
    check(envelope.phase() == gameboy::SnesDspEnvelope::Phase::release &&
              envelope.value() == 0,
          "persistent FLG resets the envelope again on the next sample");
    control.write_soft_reset(false);
    check(!control.next_sample().soft_reset,
          "clearing FLG is visible without waiting for another key poll");
}

void test_timed_poll_and_kon_clear() {
    gameboy::SnesDspKeyControl control;
    control.reset(false);
    control.write_kon(1);
    control.timed_phase29();
    check(control.next_timed_sample().key_on == 0,
          "first timed sample does not poll KON");
    control.timed_phase29();
    check(control.next_timed_sample().key_on == 1,
          "second timed sample polls KON");
    control.write_kon(1);
    control.timed_phase29();
    check(control.next_timed_sample().key_on == 0,
          "intermediate sample does not poll a repeated KON");
    control.timed_phase29();
    check(control.next_timed_sample().key_on == 0,
          "phase 29 clears a KON bit from the previous poll");

    control.reset(false);
    control.write_kon(1);
    control.timed_phase29();
    (void)control.next_timed_sample();
    control.timed_phase29();
    (void)control.next_timed_sample();
    control.timed_phase29();
    (void)control.next_timed_sample();
    control.timed_phase29(); // clears the previously polled bit
    control.write_kon(1);   // the same bit, now written after the clear
    check(control.next_timed_sample().key_on == 1,
          "KON written after phase-29 clear survives the phase-30 poll");

    control.reset(false);
    control.write_koff(1);
    control.timed_phase29();
    check(control.next_timed_sample().key_off == 0,
          "KOFF is not sampled on the first timed sample");
    control.timed_phase29();
    check(control.next_timed_sample().key_off == 1,
          "KOFF is sampled on the alternate timed sample");
    control.write_koff(0);
    control.timed_phase29();
    check(control.next_timed_sample().key_off == 1,
          "sampled KOFF persists between polls");
    control.timed_phase29();
    check(control.next_timed_sample().key_off == 0,
          "next timed poll observes a cleared KOFF register");

    control.reset(false);
    control.write_kon(1);
    control.write_koff(1);
    control.timed_phase29();
    (void)control.next_timed_sample();
    control.timed_phase29();
    const auto simultaneous = control.next_timed_sample();
    check(simultaneous.key_on == 1 && simultaneous.key_off == 1,
          "same timed poll samples both KON and KOFF");
    control.timed_phase29();
    const auto held = control.next_timed_sample();
    check(held.key_on == 0 && held.key_off == 1,
          "held KOFF persists after the timed KON pulse");
}

} // namespace

int main() {
    test_polling_and_register_writes();
    test_voice_precedence_and_retrigger();
    test_soft_reset_all_voices();
    test_timed_poll_and_kon_clear();
    return failures == 0 ? 0 : 1;
}
