#include "gameboy/memory_bus.hpp"
#include "gameboy/printer.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "PRINTER TEST";
    for (std::size_t index = 0; index < title.size(); ++index) {
        rom[0x134 + index] = static_cast<std::uint8_t>(title[index]);
    }
    return rom;
}

void test_printer() {
    gameboy::GameBoyPrinter printer;
    const auto send_packet = [&printer](const std::uint8_t command,
                                        const std::uint8_t compression,
                                        const std::vector<std::uint8_t>& data,
                                        const bool valid_checksum = true) {
        const auto length = static_cast<std::uint16_t>(data.size());
        auto checksum = static_cast<std::uint16_t>(
            command + compression + (length & 0xFF) + (length >> 8));
        static_cast<void>(printer.transfer(0x88));
        static_cast<void>(printer.transfer(0x33));
        static_cast<void>(printer.transfer(command));
        static_cast<void>(printer.transfer(compression));
        static_cast<void>(printer.transfer(static_cast<std::uint8_t>(length)));
        static_cast<void>(printer.transfer(static_cast<std::uint8_t>(length >> 8)));
        for (const auto byte : data) {
            checksum = static_cast<std::uint16_t>(checksum + byte);
            static_cast<void>(printer.transfer(byte));
        }
        if (!valid_checksum) ++checksum;
        static_cast<void>(printer.transfer(static_cast<std::uint8_t>(checksum)));
        static_cast<void>(printer.transfer(static_cast<std::uint8_t>(checksum >> 8)));
        const auto acknowledgement = printer.transfer(0);
        const auto status = printer.transfer(0);
        check(acknowledgement == (valid_checksum ? 0x81 : 0),
              "Game Boy Printer only acknowledges valid complete packets");
        return status;
    };

    check(send_packet(0x01, 0, {}) == 0,
          "Game Boy Printer initialization clears its status");
    std::vector<std::uint8_t> tile(16, 0);
    tile[0] = 0xFF;
    check(send_packet(0x04, 0, tile) == 0,
          "Game Boy Printer accepts uncompressed tile data");
    check(send_packet(0x04, 1, {0x8E, 0x00}) == 0x08,
          "Game Boy Printer expands compressed run-length tile data");
    check(send_packet(0x02, 0, {1, 0, 0xE4, 0x40}) == 0x08,
          "Game Boy Printer completes a print command");
    auto images = printer.take_images();
    check(images.size() == 1 && images[0].height == 8 &&
              images[0].pixels.size() == gameboy::PrinterImage::width * 8 &&
              images[0].pixels[0] == 1 && images[0].pixels[7] == 1 &&
              images[0].pixels[8] == 0,
          "Game Boy Printer converts tiles and print palettes into an image");
    const auto bitmap = gameboy::encode_printer_bmp(images[0]);
    constexpr auto bitmap_row_size = gameboy::PrinterImage::width * 4 * 3;
    const auto top_row_offset = 54 + (images[0].height * 4 - 1) * bitmap_row_size;
    check(bitmap.size() == 54 + images[0].height * 4 * bitmap_row_size &&
              bitmap[0] == 'B' && bitmap[1] == 'M' && bitmap[18] == 0x80 &&
              bitmap[19] == 0x02 && bitmap[22] == 0x20 &&
              bitmap[top_row_offset] == 170,
          "printer images encode as scaled lossless 24-bit BMP files");
    check(printer.take_images().empty(),
          "taking printer images drains the completed print queue");
    check(send_packet(0x0F, 0, {}) == 0x04,
          "Game Boy Printer reports a completed page to status inquiries");
    check(send_packet(0x0F, 0, {}, false) == 0,
          "Game Boy Printer rejects invalid packet checksums");

    gameboy::MemoryBus bus{gameboy::Cartridge{test_rom()}};
    bus.connect_printer();
    bus.write8(0xFF01, 0x88);
    bus.write8(0xFF02, 0x81);
    bus.tick(4096);
    check(bus.read8(0xFF01) == 0 && bus.take_serial_output().empty(),
          "a connected printer exchanges serial bytes without debug transcript");
}

} // namespace

int main() {
    test_printer();
    return failures == 0 ? 0 : 1;
}
