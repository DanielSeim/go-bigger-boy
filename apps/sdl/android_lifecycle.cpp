#include "android_lifecycle.hpp"

#ifdef __ANDROID__

#include "android_touch_input.hpp"
#include "audio_output.hpp"
#include "dialogs.hpp"
#include "input_mapping.hpp"
#include "input_lifecycle.hpp"
#include "presentation.hpp"
#include "settings_persistence.hpp"

#include <SDL3/SDL.h>
#include <jni.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace gbb::sdl {

void open_android_library(const bool return_to_game,
                          const std::string& running_rom) noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return;
    const auto activity_class = environment->GetObjectClass(activity);
    if (activity_class != nullptr) {
        const auto method = environment->GetMethodID(
            activity_class, "openLibrary", "(ZLjava/lang/String;)V");
        if (method != nullptr) {
            const auto path = environment->NewStringUTF(running_rom.c_str());
            environment->CallVoidMethod(activity, method,
                                         static_cast<jboolean>(return_to_game),
                                         path);
            if (path != nullptr) environment->DeleteLocalRef(path);
        }
        environment->DeleteLocalRef(activity_class);
    }
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    environment->DeleteLocalRef(activity);
}

void leave_android_game(
    std::unique_ptr<gbb::EmulatorCore>& core, gameboy::Emulator*& emulator,
    SdlResources& sdl, bool& dashboard_visible, bool& paused,
    bool& fast_forward, bool& rewind,
    std::deque<std::vector<std::uint8_t>>& rewind_history, bool& running) {
    if (core == nullptr) {
        if (confirm_exit(sdl.window)) running = false;
        return;
    }
    clear_touch_buttons(core.get(), sdl);
    sdl.android_menu_visible = false;
    sdl.android_link_menu_visible = false;
    flush_battery_safely(core.get());
    if (!confirm_exit(sdl.window)) return;

    // Keep SDL's native thread alive while the Android library is shown.
    if (emulator != nullptr) release_all_buttons(*emulator);
    stop_rumble(sdl);
    sdl.camera.close();
    core.reset();
    emulator = nullptr;
    rewind_history.clear();
    fast_forward = false;
    rewind = false;
    dashboard_visible = false;
    paused = true;
    open_android_library(false);
}

std::string persist_android_rom(const std::string& source,
                                const std::filesystem::path& preference_path,
                                const std::string& preferred_display_name) {
    if (preference_path.empty()) return source;
    const auto rom_directory = (preference_path / "roms").lexically_normal();
    const auto source_path = std::filesystem::u8path(source).lexically_normal();
    if (source_path.parent_path() == rom_directory) return source;

    std::size_t byte_count{};
    void* loaded = SDL_LoadFile(source.c_str(), &byte_count);
    if (loaded == nullptr) {
        throw std::runtime_error(std::string{"Could not import ROM: "} +
                                 SDL_GetError());
    }
    const std::unique_ptr<void, decltype(&SDL_free)> owned(loaded, SDL_free);
    const auto* bytes = static_cast<const std::uint8_t*>(loaded);
    std::uint64_t fingerprint = 14695981039346656037ULL;
    for (std::size_t index = 0; index < byte_count; ++index) {
        fingerprint ^= bytes[index];
        fingerprint *= 1099511628211ULL;
    }

    auto display_name = preferred_display_name.empty() ? source
                                                        : preferred_display_name;
    if (const auto query = display_name.find_first_of("?#");
        query != std::string::npos) display_name.resize(query);
    std::string decoded_name;
    decoded_name.reserve(display_name.size());
    for (std::size_t index = 0; index < display_name.size(); ++index) {
        if (display_name[index] == '%' && index + 2 < display_name.size() &&
            std::isxdigit(static_cast<unsigned char>(display_name[index + 1])) &&
            std::isxdigit(static_cast<unsigned char>(display_name[index + 2]))) {
            const auto digit = [](const char value) {
                if (value >= '0' && value <= '9') return value - '0';
                return std::tolower(static_cast<unsigned char>(value)) - 'a' + 10;
            };
            decoded_name.push_back(static_cast<char>(
                digit(display_name[index + 1]) * 16 +
                digit(display_name[index + 2])));
            index += 2;
        } else {
            decoded_name.push_back(display_name[index]);
        }
    }
    display_name = std::move(decoded_name);
    if (const auto separator = display_name.find_last_of("/\\:");
        separator != std::string::npos) display_name.erase(0, separator + 1);
    for (auto& character : display_name) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || std::string_view{"/\\:*?\"<>|"}.find(character) !=
                             std::string_view::npos) character = '_';
    }
    if (display_name.empty()) display_name = "game.gb";
    if (display_name.size() > 160) display_name.resize(160);

    std::filesystem::create_directories(rom_directory);
    std::ostringstream filename;
    filename << std::hex << std::setw(16) << std::setfill('0') << fingerprint
             << '-' << display_name;
    const auto destination = rom_directory / filename.str();
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes),
                 static_cast<std::streamsize>(byte_count));
    if (!output) throw std::runtime_error("Could not retain the imported ROM");
    return destination.u8string();
}

} // namespace gbb::sdl

#endif
