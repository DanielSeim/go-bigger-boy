#include "gameboy/snes_dsp_brr_stream.hpp"

#include "gameboy/snes_audio_host.hpp"

namespace gameboy {

void SnesDspBrrStream::reset() noexcept {
    decoder_.reset();
    next_address_ = 0;
}

std::uint16_t SnesDspBrrStream::directory_word(
    const std::uint8_t directory,
    const std::uint8_t source,
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

void SnesDspBrrStream::key_on(const std::uint8_t directory,
                              const std::uint8_t source) noexcept {
    // Key-on starts at the current directory entry. It does not clear the
    // decoder's history: hardware can reuse samples in the physical ring.
    next_address_ = directory_word(directory, source, 0);
}

SnesDspBrrStream::Result SnesDspBrrStream::decode_next(
    const std::uint8_t directory,
    const std::uint8_t source) noexcept {
    Result result;
    result.source_address = next_address_;
    SnesBrrDecoder::EncodedBlock encoded{};
    for (unsigned index = 0; index < encoded.size(); ++index) {
        encoded[index] = bus_.dsp_read_ram(
            static_cast<std::uint16_t>(next_address_ + index));
    }
    result.block = decoder_.decode(encoded);
    if (result.block.end) {
        next_address_ = directory_word(directory, source, 2);
        result.release_envelope = !result.block.loop;
    } else {
        next_address_ = static_cast<std::uint16_t>(next_address_ + encoded.size());
    }
    result.next_address = next_address_;
    return result;
}

} // namespace gameboy
