#pragma once

#include "gameboy/emulator.hpp"

#include <SDL3/SDL.h>

#include <chrono>

namespace gbb::sdl {

// Owns the optional Game Boy Camera capture path.  SDL resource ownership,
// permission handling, orientation correction, and grayscale conversion are
// kept out of the frontend event loop; the emulator remains the only consumer
// of the resulting 128x112 frame.
class CameraCapture {
  public:
    CameraCapture() = default;
    ~CameraCapture();

    CameraCapture(const CameraCapture&) = delete;
    CameraCapture& operator=(const CameraCapture&) = delete;

    void close() noexcept;
    void configure(const gameboy::Emulator& emulator);
    void update(gameboy::Emulator* emulator);

  private:
    SDL_Camera* camera_{};
    SDL_Surface* conversion_surface_{};
    bool mirror_{};
    bool back_facing_{};
    bool warning_shown_{};
    std::chrono::steady_clock::time_point next_frame_{};
};

} // namespace gbb::sdl
