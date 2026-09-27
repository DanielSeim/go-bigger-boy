#include "gameboy/snes_dsp_brr_group_stream.hpp"

#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_sample_ring.hpp"

#include <cstdint>

namespace gameboy {

void SnesDspBrrGroupStream::reset() noexcept {
    decoder_.reset();
    block_address_ = 0;
    group_index_ = 0;
}

std::uint16_t SnesDspBrrGroupStream::directory_word(
    const std::uint8_t directory, const std::uint8_t source,
    const unsigned word_offset) const noexcept {
    const auto address = static_cast<std::uint16_t>(
        (static_cast<unsigned>(directory) << 8) +
        (static_cast<unsigned>(source) << 2) + word_offset);
    const auto low = bus_.dsp_read_ram(address);
    const auto high = bus_.dsp_read_ram(
        static_cast<std::uint16_t>(address + 1));
    return static_cast<std::uint16_t>(
        low | (static_cast<unsigned>(high) << 8));
}

void SnesDspBrrGroupStream::key_on(const std::uint8_t directory,
                                   const std::uint8_t source) noexcept {
    block_address_ = directory_word(directory, source, 0);
    group_index_ = 0;
    // Preserve sequential prediction history for the two-argument diagnostic
    // path. decode_into_ring() reseeds from the physical ring before decoding.
}

SnesDspBrrGroupStream::Result SnesDspBrrGroupStream::decode_next_group(
    const std::uint8_t directory, const std::uint8_t source) noexcept {
    Result result;
    result.source_address = block_address_;
    result.group_index = group_index_;
    const auto header = bus_.dsp_read_ram(block_address_);
    const auto byte_offset = 1U + 2U * group_index_;
    const SnesBrrDecoder::EncodedGroup bytes{
        bus_.dsp_read_ram(static_cast<std::uint16_t>(block_address_ + byte_offset)),
        bus_.dsp_read_ram(static_cast<std::uint16_t>(block_address_ + byte_offset + 1U)),
    };
    result.samples = decoder_.decode_group(header, bytes);
    result.end = (header & 1U) != 0;
    result.loop = (header & 2U) != 0;
    result.release_envelope = result.end && !result.loop;
    result.completed_block = group_index_ == 3;
    if (result.completed_block) {
        block_address_ = result.end
            ? directory_word(directory, source, 2)
            : static_cast<std::uint16_t>(block_address_ + 9);
        group_index_ = 0;
    } else {
        ++group_index_;
    }
    result.next_address = block_address_;
    return result;
}

SnesDspBrrGroupStream::Result SnesDspBrrGroupStream::decode_into_ring(
    const std::uint8_t directory, const std::uint8_t source,
    SnesDspSampleRing& ring) noexcept {
    const auto history = ring.predictor_history();
    decoder_.seed_history(history[0], history[1]);
    auto result = decode_next_group(directory, source);
    ring.load_group(result.samples);
    return result;
}

} // namespace gameboy
