#include "gameboy/emulator.hpp"
#include "gameboy/sgb_sound.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

int failures{};

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000);
    rom[0x146] = 3; // synthetic SGB-capable cartridge header
    return rom;
}

void test_packet_decoder() {
    gameboy::SgbSoundTransfer::Payload payload{};
    // Two synthetic APU-RAM writes followed by a jump. The bytes are test
    // patterns, not a captured game's code, score or sample data.
    payload[0] = 3;
    payload[2] = 0x00;
    payload[3] = 0x2B;
    payload[4] = 0xA1;
    payload[5] = 0xB2;
    payload[6] = 0xC3;
    payload[7] = 2;
    payload[9] = 0xFE;
    payload[10] = 0xFF;
    payload[11] = 0x11;
    payload[12] = 0x22;
    payload[16] = 4;
    const auto result = gameboy::SgbSoundTransfer::parse(payload);
    check(result.valid() && result.writes.size() == 2 &&
              result.data_bytes == 5 && result.consumed_bytes == 17 &&
              result.jump_address == 0x0400,
          "SOU_TRN parser walks little-endian write and jump packets");
    check(result.writes[0].destination == 0x2B00 &&
              result.writes[0].source_offset == 4 &&
              result.writes[0].size == 3 &&
              result.writes[1].destination == 0xFFFE &&
              result.writes[1].source_offset == 11 &&
              result.writes[1].size == 2,
          "SOU_TRN parser reports bounded source and destination ranges");
    const auto digest = gameboy::SgbSoundTransfer::digest(payload);
    payload[4] ^= 1;
    check(gameboy::SgbSoundTransfer::digest(payload) != digest,
          "sound transfer digest includes the full payload");

    payload.fill(0);
    payload[0] = 0xFF;
    payload[1] = 0x0F;
    check(gameboy::SgbSoundTransfer::parse(payload).error ==
              gameboy::SgbSoundTransfer::Error::truncated_data,
          "SOU_TRN parser rejects a write longer than the 4 KiB latch");
    payload.fill(0);
    payload[0] = 3;
    payload[2] = 0xFE;
    payload[3] = 0xFF;
    check(gameboy::SgbSoundTransfer::parse(payload).error ==
              gameboy::SgbSoundTransfer::Error::destination_overflow,
          "SOU_TRN parser rejects an APU-RAM write that wraps at 64 KiB");
    payload.fill(0);
    payload[0] = 0xFA;
    payload[1] = 0x0F; // 4090 data bytes after the 4-byte header
    check(gameboy::SgbSoundTransfer::parse(payload).error ==
              gameboy::SgbSoundTransfer::Error::truncated_header,
          "SOU_TRN parser rejects an incomplete final packet header");
    payload[0] = 0xFC;
    check(gameboy::SgbSoundTransfer::parse(payload).error ==
              gameboy::SgbSoundTransfer::Error::missing_jump,
          "SOU_TRN parser requires a terminating jump packet");
}

void test_stereo_mixer() {
    gameboy::SgbHostAudioMixer mixer;
    check(!mixer.enqueue({1}) && mixer.queued_samples() == 0,
          "host PCM must contain complete stereo frames");
    check(mixer.enqueue({100, -100, 32000, -32000}),
          "host PCM accepts complete stereo frames");
    std::vector<std::int16_t> gb{200, 50};
    mixer.mix_into(gb);
    check(gb == std::vector<std::int16_t>({300, -50}) &&
              mixer.queued_samples() == 2,
          "host PCM mixes per channel and retains later frames");
    gb = {2000, -2000, 7, 9};
    mixer.mix_into(gb);
    check(gb == std::vector<std::int16_t>({32767, -32768, 7, 9}) &&
              mixer.queued_samples() == 0,
          "host PCM saturates at int16 bounds without changing unmatched frames");
    check(!mixer.enqueue(std::vector<std::int16_t>(
              gameboy::SgbHostAudioMixer::max_buffered_samples + 2)),
          "host PCM queue has a hard two-second bound");
    check(mixer.enqueue({1, 2}), "host PCM queue remains usable after rejection");
    mixer.clear();
    check(mixer.queued_samples() == 0, "host PCM queue clears on reset");
}

void test_bus_audio_boundary() {
    gameboy::Emulator baseline{gameboy::Cartridge{test_rom()},
                               gameboy::HardwareModel::sgb2};
    gameboy::Emulator mixed{gameboy::Cartridge{test_rom()},
                            gameboy::HardwareModel::sgb2};
    baseline.bus().tick(10'000);
    mixed.bus().tick(10'000);
    auto expected = baseline.take_audio_samples();
    check(expected.size() >= 2 && expected.size() % 2 == 0,
          "GB APU produces stereo samples for the integration check");
    std::vector<std::int16_t> host(expected.size(), 1);
    check(mixed.bus().submit_sgb_host_audio(host),
          "SGB host PCM can enter the public core audio path");
    auto actual = mixed.take_audio_samples();
    for (auto& sample : expected) {
        sample = std::min<std::int32_t>(sample + 1,
                                         std::numeric_limits<std::int16_t>::max());
    }
    check(actual == expected, "SGB host PCM mixes with normal Game Boy audio");
    mixed.set_audio_enabled(false);
    check(!mixed.bus().submit_sgb_host_audio({1, 2}) &&
              mixed.take_audio_samples().empty(),
          "muting drops host PCM as well as Game Boy output");
    mixed.set_audio_enabled(true);
    check(mixed.bus().submit_sgb_host_audio({1, 2}),
          "host PCM accepts input after unmuting");
    const auto snapshot = mixed.save_state();
    gameboy::Emulator comparison{gameboy::Cartridge{test_rom()},
                                 gameboy::HardwareModel::sgb2};
    comparison.load_state(snapshot);
    mixed.load_state(snapshot);
    mixed.bus().tick(10'000);
    comparison.bus().tick(10'000);
    const auto restored_audio = mixed.take_audio_samples();
    check(restored_audio.size() > 2 &&
              restored_audio == comparison.take_audio_samples(),
          "save-state restore drops pending host PCM without changing GB audio");
    gameboy::Emulator dmg{gameboy::Cartridge{test_rom()},
                          gameboy::HardwareModel::dmg};
    check(!dmg.bus().submit_sgb_host_audio({1, 2}),
          "non-SGB hardware never accepts SNES-side audio");
}

} // namespace

int main() {
    test_packet_decoder();
    test_stereo_mixer();
    test_bus_audio_boundary();
    return failures == 0 ? 0 : 1;
}
