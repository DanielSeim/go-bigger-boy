#pragma once

#include "gbb/core.hpp"

#include <SDL3/SDL.h>

namespace gbb::sdl {

// Owns the SDL playback stream and the frontend's latency policy. The core
// only produces samples; queue trimming, fast-forward downsampling, and SDL
// errors stay in this presentation boundary.
class AudioOutput {
  public:
    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void close() noexcept;
    void clear() noexcept;
    void set_enabled(bool enabled) noexcept;
    void submit(gbb::EmulatorCore* core, bool fast_forward = false,
                unsigned fast_forward_factor = 4,
                bool maintain_during_link_wait = false);
    [[nodiscard]] bool available() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] int queued_bytes() const noexcept;
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

  private:
    SDL_AudioStream* stream_{};
    // Start playback only after a small cushion has been queued. Link
    // sessions can spend a few milliseconds servicing packet bursts on the
    // emulation thread; without a cushion those harmless scheduling gaps
    // become audible underruns.
    bool playback_started_{};
    bool enabled_{true};
};

} // namespace gbb::sdl
