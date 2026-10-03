#include "gameboy/sgb_audio_mixer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gameboy {
SgbAudioMixer::SgbAudioMixer(unsigned hz, unsigned gb, unsigned snes)
    : output_hz_(hz), gb_gain_q15_(gb), snes_gain_q15_(snes) {
    if (hz < 8000 || hz > 48000 || gb > 32768 || snes > 32768)
        throw std::invalid_argument("invalid SGB audio rate or Q15 gain");
    maximum_sample_clock_ = std::numeric_limits<std::uint64_t>::max() / output_hz_;
    maximum_advance_clock_ = (std::numeric_limits<std::uint64_t>::max() - master_hz) / output_hz_;
}
bool SgbAudioMixer::push(Source source, std::uint64_t clock, StereoSample sample) noexcept {
    const auto index = static_cast<unsigned>(source);
    if (index >= streams_.size() || clock > maximum_sample_clock_ ||
        clock*output_hz_ < time_) return false;
    auto& s = streams_[index];
    if (s.count == capacity || clock < s.last_clock) return false;
    s.events[(s.head+s.count)%capacity] = {clock,sample};
    ++s.count; s.last_clock = clock;
    next_event_ = std::min(next_event_,clock*output_hz_);
    return true;
}
bool SgbAudioMixer::reset_gb_at(std::uint64_t clock) noexcept {
    if (clock > maximum_sample_clock_ ||
        clock*output_hz_ < time_) return false;
    auto& s = streams_[0];
    auto count = s.count;
    while (count && s.events[(s.head+count-1)%capacity].clock >= clock) --count;
    if (count == capacity) return false;
    while (s.count > count) { s.events[(s.head+--s.count)%capacity] = {}; }
    s.last_clock = clock;
    refresh_cache();
    return push(Source::gb, clock, {});
}
void SgbAudioMixer::refresh_cache() noexcept {
    next_event_ = std::numeric_limits<std::uint64_t>::max();
    for (const auto& s:streams_)
        if (s.count) next_event_ = std::min(next_event_,s.events[s.head].clock*output_hz_);
    left_level_ = std::int64_t(streams_[0].held.left)*gb_gain_q15_ +
                  std::int64_t(streams_[1].held.left)*snes_gain_q15_;
    right_level_ = std::int64_t(streams_[0].held.right)*gb_gain_q15_ +
                   std::int64_t(streams_[1].held.right)*snes_gain_q15_;
}
bool SgbAudioMixer::advance_to(std::uint64_t clock) noexcept {
    if (clock > maximum_advance_clock_) return false;
    const auto target = clock*output_hz_;
    if (target < time_) return false;
    if (target < (produced_+1)*master_hz && target < next_event_) {
        const auto duration = static_cast<std::int64_t>(target-time_);
        left_area_ += left_level_*duration; right_area_ += right_level_*duration;
        time_ = target; return true;
    }
    if (target/master_hz-produced_ > capacity-count_) return false;
    while (time_ < target) {
        // Apply all changes at this time, including equal-timestamp reset/data.
        bool changed = false;
        for (auto& s:streams_) {
            while (s.count && s.events[s.head].clock*output_hz_ <= time_) {
                s.held = s.events[s.head].sample; s.events[s.head] = {};
                s.head = (s.head+1)%capacity; --s.count;
                changed = true;
            }
        }
        if (changed) refresh_cache();
        const auto boundary = (produced_+1)*master_hz;
        auto end = std::min(target,boundary);
        end = std::min(end,next_event_);
        const auto duration = static_cast<std::int64_t>(end-time_);
        left_area_ += left_level_*duration; right_area_ += right_level_*duration;
        time_ = end;
        if (end == boundary) {
            constexpr auto divisor = std::int64_t(master_hz)*32768;
            const auto round = [](std::int64_t value) {
                return value < 0 ? -((-value+divisor/2)/divisor) : (value+divisor/2)/divisor;
            };
            const auto l = round(left_area_), r = round(right_area_);
            if (l < -32768 || l > 32767 || r < -32768 || r > 32767) ++clipped_;
            pcm_[(head_+count_)%capacity] = {
                static_cast<std::int16_t>(std::clamp<std::int64_t>(l,-32768,32767)),
                static_cast<std::int16_t>(std::clamp<std::int64_t>(r,-32768,32767))};
            ++count_; ++produced_; left_area_ = right_area_ = 0;
        }
    }
    // Consume events exactly on the target, before the next output interval.
    bool changed = false;
    for (auto& s:streams_) {
        while (s.count && s.events[s.head].clock*output_hz_ == time_) {
            s.held = s.events[s.head].sample; s.events[s.head] = {};
            s.head = (s.head+1)%capacity; --s.count;
            changed = true;
        }
    }
    if (changed) refresh_cache();
    return true;
}
bool SgbAudioMixer::pop_sample(StereoSample& sample) noexcept {
    if (!count_) return false;
    sample = pcm_[head_]; pcm_[head_] = {};
    head_ = (head_+1)%capacity; --count_; return true;
}
bool SgbAudioMixer::validate() const noexcept {
    if (output_hz_ < 8000 || output_hz_ > 48000 || gb_gain_q15_ > 32768 || snes_gain_q15_ > 32768 ||
        head_ >= capacity || count_ > capacity || produced_ < count_ || clipped_ > produced_ ||
        produced_ != time_/master_hz) return false;
    const auto area_bound = std::int64_t(time_%master_hz)*32768*(gb_gain_q15_+snes_gain_q15_);
    if (left_area_ < -area_bound || left_area_ > area_bound ||
        right_area_ < -area_bound || right_area_ > area_bound) return false;
    for (const auto& s:streams_) {
        if (s.head >= capacity || s.count > capacity ||
            s.last_clock > std::numeric_limits<std::uint64_t>::max()/output_hz_) return false;
        auto previous = time_/output_hz_;
        for (std::size_t n=0;n<s.count;++n) {
            const auto clock = s.events[(s.head+n)%capacity].clock;
            if (clock < previous || clock > s.last_clock || clock*output_hz_ <= time_) return false;
            previous = clock;
        }
    }
    return true;
}
} // namespace gameboy
