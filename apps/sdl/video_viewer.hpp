#pragma once

#ifndef __ANDROID__

#include <SDL3/SDL.h>

#include <memory>

namespace gameboy {
class Emulator;
struct DisplayPalette;
}

namespace gbb::sdl {

class VideoViewerImplementation;

class VideoViewer final {
public:
    VideoViewer();
    ~VideoViewer();
    VideoViewer(const VideoViewer&) = delete;
    VideoViewer& operator=(const VideoViewer&) = delete;

    [[nodiscard]] bool visible() const noexcept;
    void open(SDL_Window* parent);
    void close() noexcept;
    bool handle_event(const SDL_Event& event, const gameboy::Emulator* emulator);
    void present(const gameboy::Emulator* emulator,
                 const gameboy::DisplayPalette& palette);

private:
    std::unique_ptr<VideoViewerImplementation> implementation_;
};

} // namespace gbb::sdl

#endif
