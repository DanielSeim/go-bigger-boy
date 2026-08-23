#include "gameboy/printer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace gameboy {
namespace {
constexpr std::uint8_t magic_1 = 0x88;
constexpr std::uint8_t magic_2 = 0x33;
constexpr std::uint8_t command_initialize = 0x01;
constexpr std::uint8_t command_print = 0x02;
constexpr std::uint8_t command_data = 0x04;
constexpr std::uint8_t command_break = 0x08;
constexpr std::uint8_t command_status = 0x0F;
constexpr std::uint8_t status_checksum_error = 0x01;
constexpr std::uint8_t status_print_complete = 0x04;
constexpr std::uint8_t status_unprocessed_data = 0x08;
constexpr std::uint8_t status_packet_error = 0x10;
constexpr std::size_t maximum_image_data = 8192;
}

std::vector<std::uint8_t> encode_printer_bmp(const PrinterImage& image,
                                             const std::size_t scale) {
    constexpr std::array<std::uint8_t, 4> intensities{255, 170, 85, 0};
    if (scale == 0 || image.height == 0 ||
        image.pixels.size() != PrinterImage::width * image.height) {
        throw std::invalid_argument("Cannot encode an invalid printer image");
    }
    if (PrinterImage::width > std::numeric_limits<std::size_t>::max() / scale ||
        image.height > std::numeric_limits<std::size_t>::max() / scale) {
        throw std::overflow_error("Printer image dimensions overflow");
    }
    const auto width = PrinterImage::width * scale;
    const auto height = image.height * scale;
    if (width > (std::numeric_limits<std::size_t>::max() - 3) / 3) {
        throw std::overflow_error("Printer image row size overflows");
    }
    const auto row_size = (width * 3 + 3) & ~std::size_t{3};
    if (height > (std::numeric_limits<std::size_t>::max() - 54) / row_size) {
        throw std::overflow_error("Printer image file size overflows");
    }
    const auto pixel_bytes = row_size * height;
    const auto file_size = std::size_t{54} + pixel_bytes;
    if (file_size > UINT32_MAX || width > INT32_MAX || height > INT32_MAX) {
        throw std::overflow_error("Printer image exceeds the BMP format limits");
    }

    std::vector<std::uint8_t> bytes(file_size, 0);
    const auto write_u16 = [&bytes](const std::size_t offset,
                                    const std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto write_u32 = [&bytes](const std::size_t offset,
                                    const std::uint32_t value) {
        for (auto byte = 0U; byte < 4; ++byte) {
            bytes[offset + byte] =
                static_cast<std::uint8_t>(value >> (byte * 8));
        }
    };
    bytes[0] = 'B';
    bytes[1] = 'M';
    write_u32(2, static_cast<std::uint32_t>(file_size));
    write_u32(10, 54);
    write_u32(14, 40);
    write_u32(18, static_cast<std::uint32_t>(width));
    write_u32(22, static_cast<std::uint32_t>(height));
    write_u16(26, 1);
    write_u16(28, 24);
    write_u32(34, static_cast<std::uint32_t>(pixel_bytes));
    write_u32(38, 2835);
    write_u32(42, 2835);

    for (auto output_y = std::size_t{0}; output_y < height; ++output_y) {
        const auto source_y = image.height - 1 - output_y / scale;
        auto output = std::size_t{54} + output_y * row_size;
        for (auto output_x = std::size_t{0}; output_x < width; ++output_x) {
            const auto shade = static_cast<std::size_t>(
                image.pixels[source_y * PrinterImage::width +
                             output_x / scale] & 3);
            const auto intensity = intensities[shade];
            bytes[output++] = intensity;
            bytes[output++] = intensity;
            bytes[output++] = intensity;
        }
    }
    return bytes;
}

std::uint8_t GameBoyPrinter::transfer(const std::uint8_t byte) {
    const auto reply = response_;
    response_ = 0;

    switch (receive_state_) {
    case ReceiveState::magic_1:
        if (byte == magic_1) receive_state_ = ReceiveState::magic_2;
        break;
    case ReceiveState::magic_2:
        if (byte == magic_2) {
            begin_packet();
            receive_state_ = ReceiveState::command;
        } else {
            receive_state_ = byte == magic_1 ? ReceiveState::magic_2
                                             : ReceiveState::magic_1;
        }
        break;
    case ReceiveState::command:
        command_ = static_cast<std::uint8_t>(byte & 0x0F);
        calculated_checksum_ = byte;
        receive_state_ = ReceiveState::compression;
        break;
    case ReceiveState::compression:
        compression_ = static_cast<std::uint8_t>(byte & 0x01);
        calculated_checksum_ = static_cast<std::uint16_t>(
            calculated_checksum_ + byte);
        receive_state_ = ReceiveState::length_low;
        break;
    case ReceiveState::length_low:
        data_length_ = byte;
        calculated_checksum_ = static_cast<std::uint16_t>(
            calculated_checksum_ + byte);
        receive_state_ = ReceiveState::length_high;
        break;
    case ReceiveState::length_high:
        data_length_ = static_cast<std::uint16_t>(
            data_length_ | ((byte & 0x03) << 8));
        calculated_checksum_ = static_cast<std::uint16_t>(
            calculated_checksum_ + byte);
        packet_data_.clear();
        packet_data_.reserve(data_length_);
        receive_state_ = data_length_ == 0 ? ReceiveState::checksum_low
                                          : ReceiveState::data;
        break;
    case ReceiveState::data:
        packet_data_.push_back(byte);
        calculated_checksum_ = static_cast<std::uint16_t>(
            calculated_checksum_ + byte);
        if (packet_data_.size() == data_length_) {
            receive_state_ = ReceiveState::checksum_low;
        }
        break;
    case ReceiveState::checksum_low:
        received_checksum_ = byte;
        receive_state_ = ReceiveState::checksum_high;
        break;
    case ReceiveState::checksum_high:
        received_checksum_ = static_cast<std::uint16_t>(
            received_checksum_ | (byte << 8));
        if (finish_packet()) {
            response_ = 0x81;
            receive_state_ = ReceiveState::acknowledge;
        } else {
            receive_state_ = ReceiveState::magic_1;
        }
        break;
    case ReceiveState::acknowledge:
        // The printer reports the state preceding this packet, then acts on
        // the command after the status byte has shifted out.
        response_ = command_ == command_initialize ? 0 : status_;
        receive_state_ = ReceiveState::status;
        break;
    case ReceiveState::status:
        execute_packet();
        receive_state_ = byte == magic_1 ? ReceiveState::magic_2
                                         : ReceiveState::magic_1;
        break;
    }
    return reply;
}

std::vector<PrinterImage> GameBoyPrinter::take_images() {
    auto images = std::move(completed_images_);
    completed_images_.clear();
    return images;
}

void GameBoyPrinter::reset() noexcept {
    receive_state_ = ReceiveState::magic_1;
    response_ = 0;
    status_ = 0;
    command_ = 0;
    compression_ = 0;
    data_length_ = 0;
    received_checksum_ = 0;
    calculated_checksum_ = 0;
    packet_data_.clear();
    image_data_.clear();
    completed_images_.clear();
}

void GameBoyPrinter::begin_packet() noexcept {
    status_ = static_cast<std::uint8_t>(status_ & ~status_checksum_error);
    command_ = 0;
    compression_ = 0;
    data_length_ = 0;
    received_checksum_ = 0;
    calculated_checksum_ = 0;
    packet_data_.clear();
}

bool GameBoyPrinter::finish_packet() noexcept {
    if (received_checksum_ != calculated_checksum_) {
        status_ = static_cast<std::uint8_t>(status_ | status_checksum_error);
        return false;
    }
    return true;
}

void GameBoyPrinter::execute_packet() {
    switch (command_) {
    case command_initialize:
        image_data_.clear();
        status_ = 0;
        break;
    case command_data:
        append_image_data();
        if ((status_ & status_packet_error) == 0 && !image_data_.empty()) {
            status_ = static_cast<std::uint8_t>(status_ |
                                                status_unprocessed_data);
        }
        break;
    case command_print:
        print_image();
        break;
    case command_break:
        image_data_.clear();
        status_ = 0;
        break;
    case command_status:
        break;
    default:
        status_ = static_cast<std::uint8_t>(status_ | status_packet_error);
        break;
    }
}

void GameBoyPrinter::append_image_data() {
    std::vector<std::uint8_t> decoded;
    if (compression_ == 0) {
        decoded = packet_data_;
    } else if (compression_ == 1) {
        for (std::size_t index = 0; index < packet_data_.size();) {
            const auto control = packet_data_[index++];
            if ((control & 0x80) == 0) {
                const auto count = static_cast<std::size_t>(control) + 1;
                if (count > packet_data_.size() - index) {
                    status_ = static_cast<std::uint8_t>(status_ |
                                                        status_packet_error);
                    return;
                }
                decoded.insert(decoded.end(), packet_data_.begin() + index,
                               packet_data_.begin() + index + count);
                index += count;
            } else {
                if (index == packet_data_.size()) {
                    status_ = static_cast<std::uint8_t>(status_ |
                                                        status_packet_error);
                    return;
                }
                const auto count = static_cast<std::size_t>(control & 0x7F) + 2;
                decoded.insert(decoded.end(), count, packet_data_[index++]);
            }
            if (decoded.size() > maximum_image_data) {
                status_ = static_cast<std::uint8_t>(status_ |
                                                    status_packet_error);
                return;
            }
        }
    } else {
        status_ = static_cast<std::uint8_t>(status_ | status_packet_error);
        return;
    }

    if (decoded.size() > maximum_image_data - image_data_.size()) {
        status_ = static_cast<std::uint8_t>(status_ | status_packet_error);
        return;
    }
    image_data_.insert(image_data_.end(), decoded.begin(), decoded.end());
}

void GameBoyPrinter::print_image() {
    if (packet_data_.size() < 4 || image_data_.empty() ||
        (image_data_.size() % 16) != 0) {
        status_ = static_cast<std::uint8_t>(status_ | status_packet_error);
        return;
    }

    const auto sheets = std::max<unsigned>(1, packet_data_[0]);
    const auto margin_before = static_cast<std::size_t>(packet_data_[1] >> 4) * 8;
    const auto margin_after = static_cast<std::size_t>(packet_data_[1] & 0x0F) * 8;
    const auto palette = packet_data_[2];
    const auto tile_count = image_data_.size() / 16;
    const auto tile_rows = (tile_count + 19) / 20;
    PrinterImage image;
    image.height = margin_before + tile_rows * 8 + margin_after;
    image.pixels.assign(PrinterImage::width * image.height, 0);

    for (std::size_t tile = 0; tile < tile_count; ++tile) {
        const auto tile_x = (tile % 20) * 8;
        const auto tile_y = margin_before + (tile / 20) * 8;
        for (std::size_t row = 0; row < 8; ++row) {
            const auto low = image_data_[tile * 16 + row * 2];
            const auto high = image_data_[tile * 16 + row * 2 + 1];
            for (std::size_t column = 0; column < 8; ++column) {
                const auto shift = 7 - column;
                const auto color = static_cast<std::uint8_t>(
                    ((low >> shift) & 1) | (((high >> shift) & 1) << 1));
                image.pixels[(tile_y + row) * PrinterImage::width + tile_x +
                             column] =
                    static_cast<std::uint8_t>((palette >> (color * 2)) & 3);
            }
        }
    }

    for (auto sheet = 0U; sheet < sheets; ++sheet) {
        completed_images_.push_back(image);
    }
    image_data_.clear();
    status_ = status_print_complete;
}

} // namespace gameboy
