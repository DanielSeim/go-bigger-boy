#pragma once

#include "gameboy/snes_brr.hpp"

#include <cstdint>

namespace gameboy {

class SnesApuBus;
class SnesDspSampleRing;

// Diagnostic BRR walker that reads two data bytes and decodes four samples
// per request, using physical APU RAM. It is not cycle-accurate: the real
// DSP reads headers every sample, pipelines directory reads, and interleaves
// each voice with the other seven voices.
class SnesDspBrrGroupStream final {
public:
    struct Result {
        SnesBrrDecoder::DecodedGroup samples{};
        std::uint16_t source_address{};
        std::uint16_t next_address{};
        std::uint8_t group_index{};
        bool end{};
        bool loop{};
        bool completed_block{};
        bool release_envelope{};
    };

    explicit SnesDspBrrGroupStream(const SnesApuBus& bus) noexcept : bus_(bus) {}
    void reset() noexcept;
    void key_on(std::uint8_t directory, std::uint8_t source) noexcept;
    [[nodiscard]] Result decode_next_group(std::uint8_t directory,
                                           std::uint8_t source) noexcept;
    // For the physical-ring path, seed BRR filter history from the next write
    // point, decode one group, and install it in the ring atomically.
    [[nodiscard]] Result decode_into_ring(std::uint8_t directory,
                                          std::uint8_t source,
                                          SnesDspSampleRing& ring) noexcept;
    [[nodiscard]] std::uint16_t next_address() const noexcept { return block_address_; }
    [[nodiscard]] std::uint8_t next_group_index() const noexcept { return group_index_; }

private:
    [[nodiscard]] std::uint16_t directory_word(std::uint8_t directory,
                                                std::uint8_t source,
                                                unsigned word_offset) const noexcept;

    const SnesApuBus& bus_;
    SnesBrrDecoder decoder_{};
    std::uint16_t block_address_{};
    std::uint8_t group_index_{};
};

} // namespace gameboy
