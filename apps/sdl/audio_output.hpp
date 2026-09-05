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
    void submit(gbb::EmulatorCore* core, bool fast_forward = false,
                unsigned fast_forward_factor = 4);
    [[nodiscard]] bool available() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] int queued_bytes() const noexcept;

  private:
    SDL_AudioStream* stream_{};
};

} // namespace gbb::sdl
