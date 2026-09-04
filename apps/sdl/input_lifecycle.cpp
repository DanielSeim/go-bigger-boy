#include "input_lifecycle.hpp"

#include "gbb/frontend_logging.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <exception>
#include <string>

namespace gbb::sdl {

void flush_battery_safely(gbb::EmulatorCore* core) noexcept {
    if (core == nullptr) return;
    try {
        core->flush_persistent_data();
    } catch (const std::exception& error) {
        gbb::log_frontend_warning(
            std::string("Could not flush battery save: ") + error.what());
    } catch (...) {
        gbb::log_frontend_warning("Could not flush battery save.");
    }
}

void flush_battery_safely(gameboy::Emulator* emulator) noexcept {
    if (emulator == nullptr) return;
    try {
        emulator->flush_battery();
    } catch (const std::exception& error) {
        gbb::log_frontend_warning(
            std::string("Could not flush battery save: ") + error.what());
    } catch (...) {
        gbb::log_frontend_warning("Could not flush battery save.");
    }
}

void stop_rumble(SdlResources& sdl) noexcept {
    if (sdl.gamepad != nullptr && sdl.rumble_output_active) {
        static_cast<void>(SDL_RumbleGamepad(sdl.gamepad, 0, 0, 0));
    }
    sdl.rumble_output_active = false;
    sdl.rumble_refresh = {};
}

void update_rumble(const gbb::EmulatorCore* core, SdlResources& sdl,
                   const bool enabled) {
    const auto desired = enabled && core != nullptr &&
                         gbb::has_capability(core->descriptor().capabilities,
                                             gbb::CoreCapability::rumble) &&
                         core->rumble_active();
    if (!desired || sdl.gamepad == nullptr) {
        stop_rumble(sdl);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < sdl.rumble_refresh) return;
    constexpr auto duration_ms = 500U;
    if (SDL_RumbleGamepad(sdl.gamepad, 0xC000, 0x6000, duration_ms)) {
        sdl.rumble_output_active = true;
        sdl.rumble_refresh = now + std::chrono::milliseconds(250);
    } else {
        sdl.rumble_output_active = false;
        sdl.rumble_refresh = now + std::chrono::seconds(5);
        if (!sdl.rumble_warning_shown) {
            gbb::log_frontend_warning(
                std::string("The connected gamepad does not provide rumble output: ") +
                SDL_GetError());
            sdl.rumble_warning_shown = true;
        }
    }
}

} // namespace gbb::sdl
