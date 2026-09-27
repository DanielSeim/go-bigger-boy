#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_brr_stream.hpp"

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

void test_directory_and_looping() {
    gameboy::SnesApuBus bus;
    constexpr std::uint16_t entry = 0x200C; // DIR=$20, SRCN=$03
    write_word(bus, entry, 0x4000);
    write_word(bus, entry + 2, 0x4100);
    bus.spc_write(0x4000, 0x10); // direct, shift 1
    bus.spc_write(0x4008, 0x87); // previous two decoded samples: -8, +7
    bus.spc_write(0x4009, 0x0B); // filter 2, end + loop
    bus.spc_write(0x4100, 0x10);
    bus.spc_write(0x4101, 0x23);

    gameboy::SnesDspBrrStream stream(bus);
    stream.key_on(0x20, 3);
    check(stream.next_address() == 0x4000,
          "key-on reads the source start pointer from DIR and SRCN");
    const auto first = stream.decode_next(0x20, 3);
    check(first.source_address == 0x4000 && first.next_address == 0x4009 &&
              first.block.samples[14] == -8 && first.block.samples[15] == 7 &&
              !first.release_envelope,
          "non-end block advances nine bytes and preserves BRR history");
    const auto end = stream.decode_next(0x20, 3);
    check(end.source_address == 0x4009 && end.block.samples[0] == 20 &&
              end.block.end && end.block.loop && end.next_address == 0x4100 &&
              !end.release_envelope,
          "end+loop block jumps to the directory loop pointer without releasing");
    const auto restart = stream.decode_next(0x20, 3);
    check(restart.source_address == 0x4100 &&
              restart.block.samples[0] == 2 && restart.block.samples[1] == 3,
          "loop-point BRR data is read and decoded from APU RAM");

    write_word(bus, entry, 0x4009);
    write_word(bus, entry + 2, 0x4200);
    bus.spc_write(0x4009, 0x09); // filter 2, end without loop
    stream.key_on(0x20, 3);
    const auto stop = stream.decode_next(0x20, 3);
    check(stop.source_address == 0x4009 && stop.block.end &&
              !stop.block.loop && stop.release_envelope &&
              stop.next_address == 0x4200,
          "end without loop requests envelope release but still redirects BRR decoding");
}

void test_directory_updates_and_physical_ram() {
    gameboy::SnesApuBus bus;
    gameboy::SnesApuBus::IplRom ipl{};
    ipl[0] = 0xEE;
    bus.install_ipl(ipl);
    bus.spc_write(0xFFC0, 0x42);
    bus.host_write_port(0, 0x11);
    bus.spc_write(0xF4, 0x22);
    check(bus.spc_read(0xFFC0) == 0xEE &&
              bus.dsp_read_ram(0xFFC0) == 0x42 &&
              bus.spc_read(0xF4) == 0x11 &&
              bus.dsp_read_ram(0xF4) == 0x22,
          "DSP reads physical RAM below IPL and SPC700 I/O overlays");

    constexpr std::uint16_t entry_old = 0x3004; // DIR=$30, SRCN=$01
    constexpr std::uint16_t entry_new = 0x3104;
    write_word(bus, entry_old, 0x5000);
    write_word(bus, entry_old + 2, 0x5100);
    write_word(bus, entry_new, 0x6000);
    write_word(bus, entry_new + 2, 0x6100);
    bus.spc_write(0x5000, 0x01); // end
    gameboy::SnesDspBrrStream stream(bus);
    stream.key_on(0x30, 1);
    const auto result = stream.decode_next(0x31, 1);
    check(result.source_address == 0x5000 && result.next_address == 0x6100,
          "DIR changes affect the loop pointer at end, not the active start address");
}

void test_address_wrap_and_key_on_history() {
    gameboy::SnesApuBus bus;
    write_word(bus, 0x2200, 0xFFFC);
    bus.spc_write(0xFFFC, 0x10);
    bus.spc_write(0xFFFD, 0x23);
    gameboy::SnesDspBrrStream stream(bus);
    stream.key_on(0x22, 0);
    const auto wrapped = stream.decode_next(0x22, 0);
    check(wrapped.source_address == 0xFFFC && wrapped.next_address == 0x0005 &&
              wrapped.block.samples[0] == 2 && wrapped.block.samples[1] == 3,
          "BRR block addresses wrap through 64 KiB physical APU RAM");

    write_word(bus, 0x2200, 0x4000);
    bus.spc_write(0x4000, 0x10);
    bus.spc_write(0x4008, 0x87);
    stream.key_on(0x22, 0);
    (void)stream.decode_next(0x22, 0);
    write_word(bus, 0x2200, 0x5000);
    bus.spc_write(0x5000, 0x08); // filter 2, no new residual
    stream.key_on(0x22, 0);
    check(stream.decode_next(0x22, 0).block.samples[0] == 20,
          "key-on changes sample source without clearing physical BRR filter history");
    stream.reset();
    stream.key_on(0x22, 0);
    check(stream.decode_next(0x22, 0).block.samples[0] == 0,
          "explicit power-on reset clears decoder history");
}

} // namespace

int main() {
    test_directory_and_looping();
    test_directory_updates_and_physical_ram();
    test_address_wrap_and_key_on_history();
    return failures == 0 ? 0 : 1;
}
