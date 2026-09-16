#include "tas_editor.hpp"

#ifndef __ANDROID__

#define TasEditor TasEditorImplementation
#include "tas_editor_impl.hpp"
#undef TasEditor

#include <memory>
#include <utility>

namespace gbb::sdl {

TasEditor::TasEditor()
    : implementation_(std::make_unique<TasEditorImplementation>()) {}
TasEditor::~TasEditor() = default;
bool TasEditor::visible() const noexcept { return implementation_->visible(); }
bool TasEditor::take_save_request() noexcept { return implementation_->take_save_request(); }
bool TasEditor::take_replay_request() noexcept { return implementation_->take_replay_request(); }
bool TasEditor::take_new_request() noexcept { return implementation_->take_new_request(); }
const std::vector<std::uint8_t>& TasEditor::frames() const noexcept {
    return implementation_->frames();
}
const std::vector<std::uint8_t>& TasEditor::start_state() const noexcept {
    return implementation_->start_state();
}
bool TasEditor::has_unsaved_changes() const noexcept {
    return implementation_->has_unsaved_changes();
}
void TasEditor::mark_saved() { implementation_->mark_saved(); }
const std::string& TasEditor::status() const noexcept {
    return implementation_->status();
}
void TasEditor::set_status(std::string status) {
    implementation_->set_status(std::move(status));
}
std::uint64_t TasEditor::fingerprint() const noexcept {
    return implementation_->fingerprint();
}
bool TasEditor::close_with_confirmation() {
    return implementation_->close_with_confirmation();
}
void TasEditor::open(SDL_Window* parent, gameboy::Emulator& emulator) {
    implementation_->open(parent, emulator);
}
void TasEditor::reset_from(gameboy::Emulator& emulator) {
    implementation_->reset_from(emulator);
}
void TasEditor::close() noexcept { implementation_->close(); }
bool TasEditor::handle_event(const SDL_Event& event) {
    return implementation_->handle_event(event);
}
void TasEditor::present() { implementation_->present(); }

} // namespace gbb::sdl

#endif
