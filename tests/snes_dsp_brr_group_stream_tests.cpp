#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_brr.hpp"
#include "gameboy/snes_dsp_brr_group_stream.hpp"
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

void write_word(gameboy::SnesApuBus& bus, const std::uint16_t address,
                const std::uint16_t value) {
    bus.spc_write(address, static_cast<std::uint8_t>(value));
    bus.spc_write(static_cast<std::uint16_t>(address + 1),
                  static_cast<std::uint8_t>(value >> 8));
}

void test_group_reads_and_ring_handoff() {
    gameboy::SnesApuBus bus;
    write_word(bus, 0x2004, 0x3000);
    bus.spc_write(0x3000, 0x10); // shift 1, direct filter
    bus.spc_write(0x3001, 0x12);
    bus.spc_write(0x3002, 0x34);
    gameboy::SnesDspBrrGroupStream stream(bus);
    gameboy::SnesDspSampleRing ring;
    stream.key_on(0x20, 1);
    const auto first = stream.decode_next_group(0x20, 1);
    ring.load_group(first.samples);
    check(first.samples == gameboy::SnesBrrDecoder::DecodedGroup{1, 2, 3, 4} &&
              first.group_index == 0 && first.next_address == 0x3000 &&
              stream.next_group_index() == 1 && !first.completed_block,
          "first group consumes only two BRR data bytes and fills the ring");

    // A whole-block decoder would already have consumed these bytes.
    bus.spc_write(0x3003, 0x56);
    bus.spc_write(0x3004, 0x78);
    const auto second = stream.decode_next_group(0x20, 1);
    ring.load_group(second.samples);
    check(second.samples == gameboy::SnesBrrDecoder::DecodedGroup{5, 6, 7, -8} &&
              second.group_index == 1 && !second.completed_block,
          "later RAM edits affect later four-sample groups");
    const auto third = stream.decode_next_group(0x20, 1);
    ring.load_group(third.samples);
    check(ring.window() == gameboy::SnesDspSampleRing::Group{1, 2, 3, 4},
          "three sequential groups prefill the physical ring");
    const auto fourth = stream.decode_next_group(0x20, 1);
    check(fourth.completed_block && fourth.next_address == 0x3009 &&
              stream.next_group_index() == 0,
          "fourth group advances to the next nine-byte BRR block");
}

void test_filter_equivalence_and_loop_pointer() {
    gameboy::SnesApuBus bus;
    write_word(bus, 0x2208, 0x4000);
    write_word(bus, 0x220A, 0x4100);
    gameboy::SnesBrrDecoder::EncodedBlock block{
        0x1B, 0x17, 0x8F, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
    }; // shift 1, filter 2, end + loop
    for (unsigned i = 0; i < block.size(); ++i) {
        bus.spc_write(static_cast<std::uint16_t>(0x4000 + i), block[i]);
    }
    gameboy::SnesBrrDecoder block_decoder;
    const auto expected = block_decoder.decode(block);
    gameboy::SnesDspBrrGroupStream stream(bus);
    stream.key_on(0x22, 2);
    for (unsigned group = 0; group < 4; ++group) {
        if (group == 3) write_word(bus, 0x220A, 0x4200);
        const auto result = stream.decode_next_group(0x22, 2);
        for (unsigned i = 0; i < result.samples.size(); ++i) {
            check(result.samples[i] == expected.samples[group * 4 + i],
                  "group-wise filter history equals whole-block decoding");
        }
        check(result.end && result.loop && !result.release_envelope &&
                  result.completed_block == (group == 3),
              "loop flag is visible for every group but pointer advances only at block end");
        if (group == 3) {
            check(result.next_address == 0x4200,
                  "end block uses live directory loop pointer at its boundary");
        }
    }
    bus.spc_write(0x4200, 0x10);
    bus.spc_write(0x4201, 0x23);
    check(stream.decode_next_group(0x22, 2).source_address == 0x4200,
          "next group reads the redirected loop block");
}

void test_immediate_release_and_wrap() {
    gameboy::SnesApuBus bus;
    write_word(bus, 0x2300, 0xFFFC);
    write_word(bus, 0x2302, 0x5000);
    bus.spc_write(0xFFFC, 0x10);
    bus.spc_write(0xFFFD, 0x12);
    bus.spc_write(0xFFFE, 0x34);
    gameboy::SnesDspBrrGroupStream stream(bus);
    stream.key_on(0x23, 0);
    const auto first = stream.decode_next_group(0x23, 0);
    check(first.samples == gameboy::SnesBrrDecoder::DecodedGroup{1, 2, 3, 4} &&
              !first.release_envelope,
          "group reader wraps through physical 64 KiB APU RAM");
    bus.spc_write(0xFFFC, 0x11); // end without loop, read on this group
    bus.spc_write(0xFFFF, 0x56);
    bus.spc_write(0x0000, 0x78);
    const auto second = stream.decode_next_group(0x23, 0);
    check(second.samples == gameboy::SnesBrrDecoder::DecodedGroup{5, 6, 7, -8} &&
              second.release_envelope && !second.completed_block,
          "live end-without-loop header requests release before block completion");
    (void)stream.decode_next_group(0x23, 0);
    const auto last = stream.decode_next_group(0x23, 0);
    check(last.completed_block && last.next_address == 0x5000,
          "end-without-loop still redirects BRR reads after the final group");
}

void test_physical_ram_under_ipl() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xEE;
    bus.install_ipl(ipl);
    write_word(bus, 0x2400, 0xFFC0);
    bus.spc_write(0xFFC0, 0x10);
    bus.spc_write(0xFFC1, 0x12);
    bus.spc_write(0xFFC2, 0x34);
    gameboy::SnesDspBrrGroupStream stream(bus);
    stream.key_on(0x24, 0);
    check(bus.spc_read(0xFFC0) == 0xEE &&
              stream.decode_next_group(0x24, 0).samples ==
                  gameboy::SnesBrrDecoder::DecodedGroup{1, 2, 3, 4},
          "DSP group reads physical APU RAM beneath the SPC700 IPL overlay");
}

} // namespace

int main() {
    test_group_reads_and_ring_handoff();
    test_filter_equivalence_and_loop_pointer();
    test_immediate_release_and_wrap();
    test_physical_ram_under_ipl();
    return failures == 0 ? 0 : 1;
}
