#include "cheat_manager.hpp"
#include "gameboy/gameshark.hpp"

#ifndef __ANDROID__

#define CheatManager CheatManagerImplementation
#include "cheat_manager_impl.hpp"
#undef CheatManager

#include <memory>

namespace gbb::sdl {

CheatManager::CheatManager()
    : implementation_(std::make_unique<CheatManagerImplementation>()) {}
CheatManager::~CheatManager() = default;
void CheatManager::load(const std::filesystem::path& preference_directory,
                        const gameboy::RomMetadata& metadata) {
    implementation_->load(preference_directory, metadata);
}
void CheatManager::apply(gameboy::Emulator& emulator) const {
    implementation_->apply(emulator);
}
bool CheatManager::visible() const noexcept { return implementation_->visible(); }
bool CheatManager::fetching() const noexcept { return implementation_->fetching(); }
bool CheatManager::take_fetch_request() noexcept {
    return implementation_->take_fetch_request();
}
void CheatManager::start_fetch() { implementation_->start_fetch(); }
bool CheatManager::poll_fetch() { return implementation_->poll_fetch(); }
std::optional<std::string> CheatManager::take_fetch_error() {
    return implementation_->take_fetch_error();
}
void CheatManager::open(SDL_Window* parent) { implementation_->open(parent); }
void CheatManager::close() noexcept { implementation_->close(); }
bool CheatManager::handle_event(const SDL_Event& event) {
    return implementation_->handle_event(event);
}
void CheatManager::fetch_archive() { implementation_->fetch_archive(); }
void CheatManager::present() { implementation_->present(); }

} // namespace gbb::sdl

#endif
