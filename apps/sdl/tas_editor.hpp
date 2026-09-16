#pragma once

#ifndef __ANDROID__

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gameboy {
class Emulator;
}

namespace gbb::sdl {

class TasEditorImplementation;

class TasEditor final {
public:
    TasEditor();
    ~TasEditor();
    TasEditor(const TasEditor&) = delete;
    TasEditor& operator=(const TasEditor&) = delete;

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool take_save_request() noexcept;
    [[nodiscard]] bool take_replay_request() noexcept;
    [[nodiscard]] bool take_new_request() noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& frames() const noexcept;
    [[nodiscard]] const std::vector<std::uint8_t>& start_state() const noexcept;
    [[nodiscard]] bool has_unsaved_changes() const noexcept;
    void mark_saved();
    [[nodiscard]] const std::string& status() const noexcept;
    void set_status(std::string status);
    [[nodiscard]] std::uint64_t fingerprint() const noexcept;
    [[nodiscard]] bool close_with_confirmation();
    void open(SDL_Window* parent, gameboy::Emulator& emulator);
    void reset_from(gameboy::Emulator& emulator);
    void close() noexcept;
    bool handle_event(const SDL_Event& event);
    void present();

private:
    std::unique_ptr<TasEditorImplementation> implementation_;
};

} // namespace gbb::sdl

#endif
