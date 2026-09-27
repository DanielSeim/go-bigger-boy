#include "gameboy/sgb_sound.hpp"

#include <algorithm>
#include <limits>

namespace gameboy {
namespace {

std::uint16_t little_u16(const SgbSoundTransfer::Payload& payload,
                         const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(payload[offset] |
                                      (static_cast<unsigned>(payload[offset + 1]) << 8));
}

} // namespace

SgbSoundTransfer::Result SgbSoundTransfer::parse(const Payload& payload) {
    Result result;
    std::size_t offset = 0;
    while (offset < payload.size()) {
        if (payload.size() - offset < 4) {
            result.error = Error::truncated_header;
            result.consumed_bytes = offset;
            return result;
        }
        const auto size = little_u16(payload, offset);
        const auto destination = little_u16(payload, offset + 2);
        offset += 4;
        if (size == 0) {
            result.jump_address = destination;
            result.consumed_bytes = offset;
            return result;
        }
        if (size > payload.size() - offset) {
            result.error = Error::truncated_data;
            result.consumed_bytes = offset;
            return result;
        }
        if (static_cast<std::uint32_t>(destination) + size > 0x10000U) {
            result.error = Error::destination_overflow;
            result.consumed_bytes = offset;
            return result;
        }
        result.writes.push_back({destination, static_cast<std::uint16_t>(offset), size});
        result.data_bytes += size;
        offset += size;
    }
    result.error = Error::missing_jump;
    result.consumed_bytes = offset;
    return result;
}

std::uint64_t SgbSoundTransfer::digest(const Payload& payload) noexcept {
    auto hash = UINT64_C(14695981039346656037);
    for (const auto byte : payload) {
        hash = (hash ^ byte) * UINT64_C(1099511628211);
    }
    return hash;
}

std::string_view SgbSoundTransfer::error_name(const Error error) noexcept {
    switch (error) {
    case Error::none: return "none";
    case Error::truncated_header: return "truncated-header";
    case Error::truncated_data: return "truncated-data";
    case Error::destination_overflow: return "destination-overflow";
    case Error::missing_jump: return "missing-jump";
    }
    return "unknown";
}

bool SgbHostAudioMixer::enqueue(
    const std::vector<std::int16_t>& stereo_samples) {
    if ((stereo_samples.size() & 1U) != 0 ||
        stereo_samples.size() > max_buffered_samples - queued_samples()) {
        return false;
    }
    if (read_offset_ == samples_.size()) {
        samples_.clear();
        read_offset_ = 0;
    } else if (read_offset_ >= 4096) {
        samples_.erase(samples_.begin(), samples_.begin() +
                       static_cast<std::ptrdiff_t>(read_offset_));
        read_offset_ = 0;
    }
    samples_.insert(samples_.end(), stereo_samples.begin(), stereo_samples.end());
    return true;
}

void SgbHostAudioMixer::mix_into(
    std::vector<std::int16_t>& gb_stereo_samples) noexcept {
    const auto count = std::min(gb_stereo_samples.size() & ~std::size_t{1},
                                queued_samples());
    for (std::size_t index = 0; index < count; ++index) {
        const auto mixed = static_cast<std::int32_t>(gb_stereo_samples[index]) +
                           samples_[read_offset_ + index];
        gb_stereo_samples[index] = static_cast<std::int16_t>(
            std::clamp(mixed,
                       static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
                       static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max())));
    }
    read_offset_ += count;
    if (read_offset_ == samples_.size()) clear();
}

void SgbHostAudioMixer::clear() noexcept {
    samples_.clear();
    read_offset_ = 0;
}

std::size_t SgbHostAudioMixer::queued_samples() const noexcept {
    return samples_.size() - read_offset_;
}

} // namespace gameboy
