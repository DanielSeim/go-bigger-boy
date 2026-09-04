#pragma once

#include "gameboy/save_state_error.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace gameboy::save_state_format {

constexpr std::size_t maximum_serial_output = 1024 * 1024;

class Writer {
public:
    void u8(const std::uint8_t value) { bytes_.push_back(value); }
    void boolean(const bool value) { u8(value ? 1 : 0); }
    void u16(const std::uint16_t value) {
        u8(static_cast<std::uint8_t>(value));
        u8(static_cast<std::uint8_t>(value >> 8));
    }
    void u32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(const std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            u8(static_cast<std::uint8_t>(value >> shift));
    }
    void f32(const float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t),
                      "save states require 32-bit floats");
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void bytes(const std::uint8_t* data, const std::size_t size) {
        bytes_.insert(bytes_.end(), data, data + size);
    }
    void string(const std::string& value) {
        if (value.size() > maximum_serial_output)
            throw SaveStateError("Serial output is too large for a save state");
        u32(static_cast<std::uint32_t>(value.size()));
        bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    }
    [[nodiscard]] const std::vector<std::uint8_t>& data() const noexcept {
        return bytes_;
    }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes,
                    const std::size_t begin = 0,
                    const std::size_t size = std::numeric_limits<std::size_t>::max())
        : bytes_(bytes), position_(begin), end_(bytes.size()) {
        if (begin > bytes.size() ||
            (size < std::numeric_limits<std::size_t>::max() &&
             size > bytes.size() - begin)) {
            throw SaveStateError("Save state is truncated");
        }
        if (size < std::numeric_limits<std::size_t>::max()) end_ = begin + size;
    }
    [[nodiscard]] std::uint8_t u8() {
        require(1);
        return bytes_[position_++];
    }
    [[nodiscard]] bool boolean() {
        const auto value = u8();
        if (value > 1) throw SaveStateError("Save state contains an invalid boolean");
        return value != 0;
    }
    [[nodiscard]] std::uint16_t u16() {
        const auto low = u8();
        const auto high = u8();
        return static_cast<std::uint16_t>(low |
                                          (static_cast<std::uint16_t>(high) << 8));
    }
    [[nodiscard]] std::uint32_t u32() {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(u8()) << shift;
        return value;
    }
    [[nodiscard]] std::uint64_t u64() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(u8()) << shift;
        return value;
    }
    [[nodiscard]] float f32() {
        const auto bits = u32();
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void bytes(std::uint8_t* destination, const std::size_t size) {
        require(size);
        std::copy_n(bytes_.data() + position_, size, destination);
        position_ += size;
    }
    [[nodiscard]] std::string string() {
        const auto size = static_cast<std::size_t>(u32());
        if (size > maximum_serial_output)
            throw SaveStateError("Save state serial output is too large");
        require(size);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + position_), size);
        position_ += size;
        return value;
    }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return end_ - position_; }
    void finish() const {
        if (position_ != end_)
            throw SaveStateError("Save state contains unexpected trailing data");
    }

private:
    void require(const std::size_t size) const {
        if (size > end_ - position_)
            throw SaveStateError("Save state is truncated");
    }
    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_{};
    std::size_t end_{};
};

[[nodiscard]] inline std::uint32_t crc32(const std::uint8_t* data,
                                         const std::size_t size) noexcept {
    auto crc = UINT32_C(0xFFFFFFFF);
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? UINT32_C(0xEDB88320) : 0);
    }
    return ~crc;
}

} // namespace gameboy::save_state_format
