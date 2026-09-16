#pragma once

#ifndef __ANDROID__

#include <SDL3/SDL.h>

#include "gameboy/rom_library.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace gameboy {
class Emulator;
}

namespace gbb::sdl {

class CheatManagerImplementation;

class CheatManager final {
public:
    CheatManager();
    ~CheatManager();
    CheatManager(const CheatManager&) = delete;
    CheatManager& operator=(const CheatManager&) = delete;

    void load(const std::filesystem::path& preference_directory,
              const gameboy::RomMetadata& metadata);
    void apply(gameboy::Emulator& emulator) const;
    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool fetching() const noexcept;
    [[nodiscard]] bool take_fetch_request() noexcept;
    void start_fetch();
    [[nodiscard]] bool poll_fetch();
    [[nodiscard]] std::optional<std::string> take_fetch_error();
    void open(SDL_Window* parent);
    void close() noexcept;
    bool handle_event(const SDL_Event& event);
    void fetch_archive();
    void present();

private:
    std::unique_ptr<CheatManagerImplementation> implementation_;
};

} // namespace gbb::sdl

#endif
