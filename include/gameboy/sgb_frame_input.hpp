#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Original held-frame input policy for deterministic firmware replays.
// Not a frontend controller backend.
namespace gameboy {
class SgbFrameInput {
public:
    bool add(std::uint64_t frame, unsigned mask) noexcept {
        if (size_ == events_.size() || frame > 100000 || mask > 255 ||
            (size_ && frame <= events_[size_ - 1].frame)) return false;
        events_[size_++] = {frame, static_cast<std::uint8_t>(mask)};
        return true;
    }
    void clear() noexcept { size_ = 0; reset(); }
    void reset() noexcept { next_ = 0; held_ = 0; }
    bool advance(std::uint64_t frame) noexcept {
        if (next_ == size_ || events_[next_].frame != frame) return false;
        held_ = events_[next_++].mask;
        return true;
    }
    std::uint8_t held() const noexcept { return held_; }
    void hold(std::uint8_t mask) noexcept { held_ = mask; }
    std::size_t applied() const noexcept { return next_; }
    std::size_t size() const noexcept { return size_; }
    std::uint8_t controller(unsigned player, std::uint8_t host, bool enabled) const noexcept {
        return enabled && player == 0 ? static_cast<std::uint8_t>(~held_) : host;
    }
private:
    friend class SgbHostStateCodec;
    struct Event { std::uint64_t frame; std::uint8_t mask; };
    std::array<Event, 1024> events_{};
    std::size_t size_{}, next_{};
    std::uint8_t held_{};
};
} // namespace gameboy
