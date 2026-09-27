#include "snes_dsp_pcm_renderer.hpp"

#include "gameboy/snes_audio_host.hpp"

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

void setup_source(gameboy::SnesApuBus& bus, const std::uint8_t header) {
    bus.spc_write(0x2800, 0x00);
    bus.spc_write(0x2801, 0x80);
    bus.spc_write(0x2802, 0x00);
    bus.spc_write(0x2803, 0x80);
    bus.spc_write(0x8000, header);
    bus.spc_write(0x8001, 0x40);
    bus.spc_write(0x8002, 0x00);
    for (unsigned address = 0x8003; address <= 0x8008; ++address) {
        bus.spc_write(static_cast<std::uint16_t>(address), 0x44);
    }
}

void configure_voice(sgb_test::SnesDspPcmRenderer& renderer,
                     const unsigned index, const std::uint8_t left,
                     const std::uint8_t right) {
    const auto base = static_cast<std::uint8_t>(index * 0x10U);
    renderer.write_dsp(static_cast<std::uint8_t>(base + 0), left);
    renderer.write_dsp(static_cast<std::uint8_t>(base + 1), right);
    renderer.write_dsp(static_cast<std::uint8_t>(base + 2), 0x00);
    renderer.write_dsp(static_cast<std::uint8_t>(base + 3), 0x10);
    renderer.write_dsp(static_cast<std::uint8_t>(base + 4), 0x00);
    renderer.write_dsp(static_cast<std::uint8_t>(base + 5), 0x00);
    renderer.write_dsp(static_cast<std::uint8_t>(base + 7), 0x7f);
}

void configure_mix(sgb_test::SnesDspPcmRenderer& renderer) {
    renderer.write_dsp(0x5D, 0x28);
    renderer.write_dsp(0x0C, 0x7f);
    renderer.write_dsp(0x1C, 0x7f);
    renderer.write_dsp(0x6C, 0x20); // echo writes disabled
}

void test_one_voice_pcm_and_controls() {
    gameboy::SnesApuBus bus;
    setup_source(bus, 0x80);
    sgb_test::SnesDspPcmRenderer renderer(bus);
    configure_mix(renderer);
    configure_voice(renderer, 0, 0x7f, 0x40);
    renderer.write_dsp(0x4C, 0x01);

    check(sgb_test::SnesDspPcmRenderer::sample_rate == 32000,
          "the test path emits native-rate DSP samples");
    const auto accepted = renderer.next_sample();
    check(accepted && accepted->left == 0 && accepted->right == 0,
          "KON takes effect after the current output sample");
    for (unsigned i = 0; i < 5; ++i) {
        const auto sample = renderer.next_sample();
        check(sample && sample->left == 0 && sample->right == 0,
              "KON startup emits five silent stereo samples");
    }
    const auto sixth = renderer.next_sample();
    check(sixth && sixth->left == 178 && sixth->right == 90,
          "sixth sample has a pinned BRR/Gaussian/envelope/volume PCM value");
    renderer.write_dsp(0x6C, 0x60);
    const auto muted = renderer.next_sample();
    check(muted && muted->left == 0 && muted->right == 0,
          "FLG mute zeroes output while synthesis continues");
    renderer.write_dsp(0x6C, 0x20);
    const auto unmuted = renderer.next_sample();
    check(unmuted && unmuted->left != 0,
          "unmuting exposes the still-running BRR voice");
    renderer.write_dsp(0x6C, 0xa0);
    const auto reset = renderer.next_sample();
    check(reset && reset->left != 0,
          "soft reset is applied after the already prepared output sample");
    const auto after_reset = renderer.next_sample();
    check(after_reset && after_reset->left == 0 && after_reset->right == 0,
          "soft reset silences subsequent PCM samples");
}

void test_two_voice_mix_and_rejection() {
    gameboy::SnesApuBus bus;
    setup_source(bus, 0x80);
    sgb_test::SnesDspPcmRenderer renderer(bus);
    configure_mix(renderer);
    configure_voice(renderer, 0, 0x7f, 0x40);
    configure_voice(renderer, 1, 0x7f, 0x00);
    renderer.write_dsp(0x3D, 0x01);
    check(!renderer.next_sample(), "noise mode is rejected rather than misrendered");
    renderer.write_dsp(0x3D, 0);
    renderer.write_dsp(0x4C, 0x03);
    (void)renderer.next_sample(); // KON acceptance, before the five silent samples
    for (unsigned i = 0; i < 5; ++i) (void)renderer.next_sample();
    const auto sixth = renderer.next_sample();
    check(sixth && sixth->left == 357 && sixth->right == 90,
          "two voices mix before master volume, with independent stereo volumes");
    renderer.write_dsp(0x2C, 1);
    check(!renderer.next_sample(), "echo output is rejected until FIR mixing exists");
}

void test_brr_end_and_key_retrigger() {
    gameboy::SnesApuBus bus;
    setup_source(bus, 0x83); // ending, looping block
    sgb_test::SnesDspPcmRenderer renderer(bus);
    configure_mix(renderer);
    configure_voice(renderer, 0, 0x7f, 0x7f);
    renderer.write_dsp(0x4C, 0x01);
    for (unsigned i = 0; i < 12; ++i) (void)renderer.next_sample();
    check(renderer.endx() == 0x01,
          "completing a looping end block latches voice zero ENDX");
    renderer.write_dsp(0x4C, 0x01);
    (void)renderer.next_sample();
    (void)renderer.next_sample();
    check(renderer.endx() == 0,
          "accepted retrigger clears ENDX after the next key poll");
    renderer.write_dsp(0x7C, 0);
    check(renderer.endx() == 0, "writing zero to ENDX also clears it");
}

void test_nonlooping_end_and_keyoff() {
    gameboy::SnesApuBus bus;
    setup_source(bus, 0x81); // ending, non-looping block
    sgb_test::SnesDspPcmRenderer renderer(bus);
    configure_mix(renderer);
    configure_voice(renderer, 0, 0x7f, 0x7f);
    renderer.write_dsp(0x4C, 0x01);
    for (unsigned i = 0; i < 12; ++i) {
        const auto sample = renderer.next_sample();
        check(sample && sample->left == 0 && sample->right == 0,
              "non-looping end header suppresses audible BRR data");
    }
    check(renderer.endx() == 0x01,
          "BRR decoding and ENDX continue after immediate envelope release");

    bus.spc_write(0x8000, 0x80); // change to non-ending source
    renderer.write_dsp(0x4C, 0x01);
    (void)renderer.next_sample(); // poll and accept KON
    for (unsigned i = 0; i < 6; ++i) (void)renderer.next_sample();
    renderer.write_dsp(0x5C, 0x01);
    for (unsigned i = 0; i < 270; ++i) (void)renderer.next_sample();
    const auto released = renderer.next_sample();
    check(released && released->left == 0 && released->right == 0,
          "held KOFF eventually releases the voice to silent PCM");
}

} // namespace

int main() {
    test_one_voice_pcm_and_controls();
    test_two_voice_mix_and_rejection();
    test_brr_end_and_key_retrigger();
    test_nonlooping_end_and_keyoff();
    return failures == 0 ? 0 : 1;
}
