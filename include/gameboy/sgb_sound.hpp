#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace gameboy {

// One SOU_TRN screen transfer is 4 KiB of little-endian APU-RAM write
// packets, terminated by a zero-length jump packet. This describes the
// transfer; it does not execute SNES code or synthesize copyrighted samples.
class SgbSoundTransfer final {
public:
    static constexpr std::size_t payload_size = 0x1000;
    using Payload = std::array<std::uint8_t, payload_size>;

    enum class Error : std::uint8_t {
        none,
        truncated_header,
        truncated_data,
        destination_overflow,
        missing_jump,
    };

    struct Write {
        std::uint16_t destination{};
        std::uint16_t source_offset{};
        std::uint16_t size{};
    };

    struct Result {
        Error error{Error::none};
        std::vector<Write> writes;
        std::uint16_t jump_address{};
        std::size_t consumed_bytes{};
        std::size_t data_bytes{};
        [[nodiscard]] bool valid() const noexcept { return error == Error::none; }
    };

    [[nodiscard]] static Result parse(const Payload& payload);
    [[nodiscard]] static std::uint64_t digest(const Payload& payload) noexcept;
    [[nodiscard]] static std::string_view error_name(Error error) noexcept;
};

// Presentation-only mixing stage for a future SNES-side renderer. The input
// is already-rendered 48 kHz stereo PCM; no SGB command is approximated here.
class SgbHostAudioMixer final {
public:
    static constexpr std::size_t max_buffered_samples = 48000 * 2 * 2;

    [[nodiscard]] bool enqueue(const std::vector<std::int16_t>& stereo_samples);
    void mix_into(std::vector<std::int16_t>& gb_stereo_samples) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t queued_samples() const noexcept;

private:
    std::vector<std::int16_t> samples_;
    std::size_t read_offset_{};
};

} // namespace gameboy
