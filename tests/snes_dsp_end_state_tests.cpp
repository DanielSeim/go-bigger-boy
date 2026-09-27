#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_brr_group_stream.hpp"
#include "gameboy/snes_dsp_end_state.hpp"
#include "gameboy/snes_dsp_envelope.hpp"

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

void start_envelope(gameboy::SnesDspEnvelope& envelope) {
    gameboy::SnesDspRateClock rates;
    envelope.key_on();
    rates.advance();
    envelope.clock(rates, 0, 0, 0x7f);
}

void setup_stream(gameboy::SnesApuBus& bus, const std::uint8_t header) {
    bus.spc_write(0x2000, 0x00);
    bus.spc_write(0x2001, 0x30);
    bus.spc_write(0x2002, 0x00);
    bus.spc_write(0x2003, 0x40);
    bus.spc_write(0x3000, header);
}

void test_end_without_loop_and_kon_priority() {
    gameboy::SnesApuBus bus;
    setup_stream(bus, 0x11); // end without loop
    gameboy::SnesDspBrrGroupStream stream(bus);
    gameboy::SnesDspEndState state;
    gameboy::SnesDspEnvelope envelope;
    stream.key_on(0x20, 0);
    start_envelope(envelope);
    check(envelope.value() != 0, "fixture begins with a nonzero envelope");

    for (unsigned i = 0; i < 4; ++i) {
        const auto group = stream.decode_next_group(0x20, 0);
        state.apply_sample(2, &group, false, envelope);
        check(envelope.value() == 0 &&
                  envelope.phase() == gameboy::SnesDspEnvelope::Phase::release,
              "non-looping end header forces immediate zero and release");
        check(state.endx() == (i == 3 ? 0x04 : 0x00),
              "ENDX sets only at completion of an ending BRR block");
        if (i == 0) start_envelope(envelope);
    }
    state.apply_sample(2, nullptr, true, envelope);
    check(state.endx() == 0, "accepted KON clears only its voice ENDX bit");

    stream.key_on(0x20, 0);
    const auto first = stream.decode_next_group(0x20, 0);
    envelope.key_on();
    state.apply_sample(2, &first, true, envelope);
    check(state.endx() == 0 &&
              envelope.phase() == gameboy::SnesDspEnvelope::Phase::attack,
          "same-sample KON outranks end-header release");
}

void test_looping_end_and_global_clear() {
    gameboy::SnesApuBus bus;
    setup_stream(bus, 0x13); // end and loop
    gameboy::SnesDspBrrGroupStream stream(bus);
    gameboy::SnesDspEndState state;
    gameboy::SnesDspEnvelope envelope;
    stream.key_on(0x20, 0);
    start_envelope(envelope);
    const auto initial_value = envelope.value();
    for (unsigned i = 0; i < 4; ++i) {
        const auto group = stream.decode_next_group(0x20, 0);
        state.apply_sample(7, &group, false, envelope);
        check(envelope.value() == initial_value &&
                  envelope.phase() != gameboy::SnesDspEnvelope::Phase::release,
              "looping end block does not cut the envelope");
    }
    check(state.endx() == 0x80 && stream.next_address() == 0x4000,
          "looping end block latches ENDX and redirects BRR reads");
    gameboy::SnesDspBrrGroupStream::Result second_voice_end{};
    second_voice_end.end = true;
    second_voice_end.completed_block = true;
    state.apply_sample(1, &second_voice_end, false, envelope);
    check(state.endx() == 0x82, "ENDX retains independent voice bits");
    state.apply_sample(0, nullptr, true, envelope);
    check(state.endx() == 0x82, "other-voice KON cannot clear latched ENDX");
    state.write_endx(0);
    check(state.endx() == 0, "any ENDX write clears every latched bit");
    state.apply_sample(8, nullptr, true, envelope);
    check(state.endx() == 0, "out-of-range voice index is ignored");
}

} // namespace

int main() {
    test_end_without_loop_and_kon_priority();
    test_looping_end_and_global_clear();
    return failures == 0 ? 0 : 1;
}
