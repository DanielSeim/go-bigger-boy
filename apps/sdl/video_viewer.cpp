#include "video_viewer.hpp"

#ifndef __ANDROID__

#define VideoViewer VideoViewerImplementation
#include "video_viewer_impl.hpp"
#undef VideoViewer

#include <memory>

namespace gbb::sdl {

VideoViewer::VideoViewer()
    : implementation_(std::make_unique<VideoViewerImplementation>()) {}
VideoViewer::~VideoViewer() = default;
bool VideoViewer::visible() const noexcept { return implementation_->visible(); }
void VideoViewer::open(SDL_Window* parent) { implementation_->open(parent); }
void VideoViewer::close() noexcept { implementation_->close(); }
bool VideoViewer::handle_event(const SDL_Event& event,
                               const gameboy::Emulator* emulator) {
    return implementation_->handle_event(event, emulator);
}
void VideoViewer::present(const gameboy::Emulator* emulator,
                          const gameboy::DisplayPalette& palette) {
    implementation_->present(emulator, palette);
}

} // namespace gbb::sdl

#endif
