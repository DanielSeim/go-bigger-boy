#include "gbb/plugin_trust.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>

namespace gbb {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

constexpr std::uint32_t rotate_right(const std::uint32_t value,
                                      const unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

class Sha256 final {
public:
    Sha256() noexcept
        : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(const std::uint8_t* data, const std::size_t size) noexcept {
        total_bytes_ += size;
        std::size_t offset = 0;
        if (buffer_size_ != 0) {
            const auto copied = std::min(size, buffer_.size() - buffer_size_);
            std::copy_n(data, copied, buffer_.data() + buffer_size_);
            buffer_size_ += copied;
            offset += copied;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_.data());
                buffer_size_ = 0;
            }
        }
        while (offset + buffer_.size() <= size) {
            transform(data + offset);
            offset += buffer_.size();
        }
        if (offset < size) {
            buffer_size_ = size - offset;
            std::copy_n(data + offset, buffer_size_, buffer_.data());
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
        const auto bit_count = total_bytes_ * 8U;
        buffer_[buffer_size_++] = 0x80;
        if (buffer_size_ > 56) {
            std::fill(buffer_.begin() + buffer_size_, buffer_.end(), 0);
            transform(buffer_.data());
            buffer_size_ = 0;
        }
        std::fill(buffer_.begin() + buffer_size_, buffer_.begin() + 56, 0);
        for (unsigned index = 0; index < 8; ++index) {
            buffer_[56 + index] = static_cast<std::uint8_t>(
                bit_count >> (56U - index * 8U));
        }
        transform(buffer_.data());
        std::array<std::uint8_t, 32> digest{};
        for (unsigned index = 0; index < state_.size(); ++index) {
            for (unsigned byte = 0; byte < 4; ++byte) {
                digest[index * 4 + byte] = static_cast<std::uint8_t>(
                    state_[index] >> (24U - byte * 8U));
            }
        }
        return digest;
    }

private:
    void transform(const std::uint8_t* block) noexcept {
        std::array<std::uint32_t, 64> words{};
        for (unsigned index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24U) |
                           (static_cast<std::uint32_t>(block[index * 4 + 1]) <<
                            16U) |
                           (static_cast<std::uint32_t>(block[index * 4 + 2]) <<
                            8U) |
                           block[index * 4 + 3];
        }
        for (unsigned index = 16; index < words.size(); ++index) {
            const auto s0 = rotate_right(words[index - 15], 7) ^
                            rotate_right(words[index - 15], 18) ^
                            (words[index - 15] >> 3U);
            const auto s1 = rotate_right(words[index - 2], 17) ^
                            rotate_right(words[index - 2], 19) ^
                            (words[index - 2] >> 10U);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        auto working = state_;
        for (unsigned index = 0; index < words.size(); ++index) {
            const auto s1 = rotate_right(working[4], 6) ^
                            rotate_right(working[4], 11) ^
                            rotate_right(working[4], 25);
            const auto choice = (working[4] & working[5]) ^
                                (~working[4] & working[6]);
            const auto temp1 = working[7] + s1 + choice + round_constants[index] +
                               words[index];
            const auto s0 = rotate_right(working[0], 2) ^
                            rotate_right(working[0], 13) ^
                            rotate_right(working[0], 22);
            const auto majority = (working[0] & working[1]) ^
                                  (working[0] & working[2]) ^
                                  (working[1] & working[2]);
            const auto temp2 = s0 + majority;
            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temp1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temp1 + temp2;
        }
        for (unsigned index = 0; index < state_.size(); ++index) {
            state_[index] += working[index];
        }
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_{};
    std::uint64_t total_bytes_{};
};

} // namespace

std::optional<std::string> plugin_sha256_file(
    const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open plugin for SHA-256 verification";
        return std::nullopt;
    }
    Sha256 sha256;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()),
                   static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) {
            sha256.update(buffer.data(), static_cast<std::size_t>(count));
        }
    }
    if (!input.eof()) {
        error = "could not read plugin for SHA-256 verification";
        return std::nullopt;
    }
    constexpr char digits[] = "0123456789abcdef";
    const auto digest = sha256.finish();
    std::string result;
    result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result += digits[byte >> 4];
        result += digits[byte & 0x0f];
    }
    return result;
}

} // namespace gbb
