#include "sprite_editor.hpp"

#ifndef __ANDROID__

#define SpriteEditor SpriteEditorImplementation
#include "sprite_editor_impl.hpp"
#undef SpriteEditor

#include <memory>

namespace gbb::sdl {

SpriteEditor::SpriteEditor()
    : implementation_(std::make_unique<SpriteEditorImplementation>()) {}
SpriteEditor::~SpriteEditor() = default;
bool SpriteEditor::visible() const noexcept { return implementation_->visible(); }
bool SpriteEditor::take_save_patch_request() noexcept {
    return implementation_->take_save_patch_request();
}
bool SpriteEditor::take_load_patch_request() noexcept {
    return implementation_->take_load_patch_request();
}
bool SpriteEditor::take_export_ips_request() noexcept {
    return implementation_->take_export_ips_request();
}
bool SpriteEditor::has_unsaved_changes(const gameboy::Emulator& emulator) const {
    return implementation_->has_unsaved_changes(emulator);
}
void SpriteEditor::mark_saved(const gameboy::Emulator& emulator) {
    implementation_->mark_saved(emulator);
}
void SpriteEditor::open(SDL_Window* parent, const gameboy::Emulator& emulator) {
    implementation_->open(parent, emulator);
}
void SpriteEditor::close() noexcept { implementation_->close(); }
void SpriteEditor::reset_session() noexcept { implementation_->reset_session(); }
bool SpriteEditor::handle_event(const SDL_Event& event,
                                gameboy::Emulator* emulator) {
    return implementation_->handle_event(event, emulator);
}
void SpriteEditor::present(const gameboy::Emulator* emulator) {
    implementation_->present(emulator);
}
void SpriteEditor::save_patch(const gameboy::Emulator& emulator,
                              const std::filesystem::path& path) const {
    implementation_->save_patch(emulator, path);
}
void SpriteEditor::load_patch(gameboy::Emulator& emulator,
                              const std::filesystem::path& path) {
    implementation_->load_patch(emulator, path);
}
SpriteEditor::IpsExportResult SpriteEditor::export_ips(
    const gameboy::Emulator& emulator, const std::filesystem::path& rom_path,
    const std::filesystem::path& output_path) const {
    const auto result = implementation_->export_ips(emulator, rom_path, output_path);
    return {result.exported, result.unresolved};
}

} // namespace gbb::sdl

#endif
