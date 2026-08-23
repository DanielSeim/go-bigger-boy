#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gameboy {

struct PrinterImage {
    static constexpr std::size_t width = 160;

    std::size_t height{};
    // One 2-bit Game Boy shade per pixel: 0 is white and 3 is black.
    std::vector<std::uint8_t> pixels;
};

[[nodiscard]] std::vector<std::uint8_t> encode_printer_bmp(
    const PrinterImage& image, std::size_t scale = 4);

class GameBoyPrinter {
public:
    [[nodiscard]] std::uint8_t transfer(std::uint8_t byte);
    [[nodiscard]] std::vector<PrinterImage> take_images();
    void reset() noexcept;

private:
    enum class ReceiveState {
        magic_1,
        magic_2,
        command,
        compression,
        length_low,
        length_high,
        data,
        checksum_low,
        checksum_high,
        acknowledge,
        status,
    };

    void begin_packet() noexcept;
    [[nodiscard]] bool finish_packet() noexcept;
    void execute_packet();
    void append_image_data();
    void print_image();

    ReceiveState receive_state_{ReceiveState::magic_1};
    std::uint8_t response_{};
    std::uint8_t status_{};
    std::uint8_t command_{};
    std::uint8_t compression_{};
    std::uint16_t data_length_{};
    std::uint16_t received_checksum_{};
    std::uint16_t calculated_checksum_{};
    std::vector<std::uint8_t> packet_data_;
    std::vector<std::uint8_t> image_data_;
    std::vector<PrinterImage> completed_images_;
};

} // namespace gameboy
