#pragma once

#include "gameboy/snes_brr.hpp"

#include <cstdint>

namespace gameboy {

class SnesApuBus;

// Isolated S-DSP BRR address/loop walker. It decodes blocks from physical APU
// RAM, but does not model the DSP's 4-sample groups, key-on latency,
// interpolation, envelopes, or actual output timing.
class SnesDspBrrStream final {
public:
    struct Result {
        SnesBrrDecoder::DecodedBlock block{};
        std::uint16_t source_address{};
        std::uint16_t next_address{};
        bool release_envelope{};
    };

    explicit SnesDspBrrStream(const SnesApuBus& bus) noexcept : bus_(bus) {}
    void reset() noexcept;
    void key_on(std::uint8_t directory, std::uint8_t source) noexcept;
    [[nodiscard]] Result decode_next(std::uint8_t directory,
                                     std::uint8_t source) noexcept;
    [[nodiscard]] std::uint16_t next_address() const noexcept { return next_address_; }

private:
    [[nodiscard]] std::uint16_t directory_word(std::uint8_t directory,
                                                std::uint8_t source,
                                                unsigned word_offset) const noexcept;

    const SnesApuBus& bus_;
    SnesBrrDecoder decoder_{};
    std::uint16_t next_address_{};
};

} // namespace gameboy
