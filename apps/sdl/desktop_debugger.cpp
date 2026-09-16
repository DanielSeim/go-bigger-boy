#include "desktop_debugger.hpp"

#ifndef __ANDROID__

#define DesktopDebugger DesktopDebuggerImplementation
#include "desktop_debugger_impl.hpp"
#undef DesktopDebugger

#include <memory>

namespace gbb::sdl {

DesktopDebugger::DesktopDebugger()
    : implementation_(std::make_unique<DesktopDebuggerImplementation>()) {}

DesktopDebugger::~DesktopDebugger() = default;

bool DesktopDebugger::visible() const noexcept { return implementation_->visible(); }
bool DesktopDebugger::execution_paused() const noexcept {
    return implementation_->execution_paused();
}
bool DesktopDebugger::take_instruction_step() noexcept {
    return implementation_->take_instruction_step();
}
bool DesktopDebugger::take_frame_step() noexcept {
    return implementation_->take_frame_step();
}
bool DesktopDebugger::take_record_toggle() noexcept {
    return implementation_->take_record_toggle();
}
bool DesktopDebugger::take_replay_request() noexcept {
    return implementation_->take_replay_request();
}
bool DesktopDebugger::take_tas_request() noexcept {
    return implementation_->take_tas_request();
}
bool DesktopDebugger::take_sprite_request() noexcept {
    return implementation_->take_sprite_request();
}
bool DesktopDebugger::take_video_viewer_request() noexcept {
    return implementation_->take_video_viewer_request();
}
bool DesktopDebugger::has_breakpoints() const noexcept {
    return implementation_->has_breakpoints();
}
bool DesktopDebugger::breakpoint_at(const std::uint16_t address) const noexcept {
    return implementation_->breakpoint_at(address);
}
std::size_t DesktopDebugger::breakpoint_count() const noexcept {
    return implementation_->breakpoint_count();
}
const std::vector<std::uint16_t>& DesktopDebugger::breakpoints() const noexcept {
    return implementation_->breakpoints();
}
std::optional<std::uint16_t> DesktopDebugger::breakpoint_hit() const noexcept {
    return implementation_->breakpoint_hit();
}
bool DesktopDebugger::toggle_breakpoint(const std::uint16_t address) {
    return implementation_->toggle_breakpoint(address);
}
void DesktopDebugger::clear_breakpoints() noexcept {
    implementation_->clear_breakpoints();
}
bool DesktopDebugger::check_breakpoint(const std::uint16_t address) noexcept {
    return implementation_->check_breakpoint(address);
}
void DesktopDebugger::request_record_toggle() noexcept {
    implementation_->request_record_toggle();
}
void DesktopDebugger::request_replay() noexcept { implementation_->request_replay(); }
void DesktopDebugger::request_tas_editor() noexcept {
    implementation_->request_tas_editor();
}
void DesktopDebugger::request_sprite_editor() noexcept {
    implementation_->request_sprite_editor();
}
void DesktopDebugger::request_video_viewers() noexcept {
    implementation_->request_video_viewers();
}
void DesktopDebugger::run() noexcept { implementation_->run(); }
void DesktopDebugger::pause() noexcept { implementation_->pause(); }
void DesktopDebugger::toggle(SDL_Window* parent) { implementation_->toggle(parent); }
void DesktopDebugger::close() noexcept { implementation_->close(); }
bool DesktopDebugger::handle_event(const SDL_Event& event,
                                   gameboy::Emulator* emulator) {
    return implementation_->handle_event(event, emulator);
}
void DesktopDebugger::present(const gameboy::Emulator& emulator,
                              const gameboy::DisplayPalette& palette,
                              const InputMovie& movie) {
    implementation_->present(emulator, palette, movie);
}

} // namespace gbb::sdl

#endif
