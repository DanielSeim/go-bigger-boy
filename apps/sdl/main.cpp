#include "gameboy/emulator.hpp"
#include "gameboy/display_palette.hpp"
#include "gameboy/gameshark.hpp"
#include "gameboy/rom_library.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/audio.hpp"
#include "gbb/gameboy_scene.hpp"
#include "gbb/voxel_profile.hpp"
#ifndef __ANDROID__
#include "update_checker.hpp"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "resource.h"
#include "windows_dashboard.hpp"
#endif

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace {

#ifndef GBB_VERSION
#define GBB_VERSION "0.20.2"
#endif

#ifdef __ANDROID__
struct AndroidRomRequest {
    std::string path;
    std::string display_name;
};

std::mutex android_rom_request_mutex;
std::optional<AndroidRomRequest> android_rom_request;
// Android's system back callback runs on the Java/UI thread.  Keep the
// request as a flag and consume it on SDL's main thread so that all emulator
// state (including camera RAM) is flushed before the game is left.
std::atomic_bool android_back_requested{false};

std::optional<AndroidRomRequest> take_android_rom_request() {
    std::lock_guard<std::mutex> lock(android_rom_request_mutex);
    auto request = std::move(android_rom_request);
    android_rom_request.reset();
    return request;
}
#endif

void flush_battery_safely(gameboy::Emulator* emulator) noexcept {
    if (emulator == nullptr) return;
    try {
        emulator->flush_battery();
    } catch (const std::exception& error) {
        std::cerr << "Warning: could not flush battery save: "
                  << error.what() << '\n';
    } catch (...) {
        std::cerr << "Warning: could not flush battery save.\n";
    }
}

struct TouchControlSettings {
    float scale{1.35F};
    float opacity{0.78F};
    // Window coordinates normalized to 0..1. Each orientation has one
    // movable D-pad plus A, B, Select, and Start. The first ten values are
    // portrait; the second ten are landscape. Each control stores x, y.
    std::array<float, 20> positions{
        0.27F, 0.82F, 0.74F, 0.79F, 0.74F, 0.90F, 0.43F, 0.96F,
        0.57F, 0.96F,
        0.12F, 0.50F, 0.88F, 0.42F, 0.88F, 0.62F, 0.42F, 0.92F,
        0.58F, 0.92F};
};

[[noreturn]] void sdl_error(const std::string& action) {
    throw std::runtime_error(action + ": " + SDL_GetError());
}

#ifdef _WIN32
enum class DesktopMenuCommand : int {
    none = 0,
    open_rom = 0x8100,
    library,
    save_state,
    load_state,
    exit_app,
    pause,
    reset,
    fullscreen,
    controls,
    gameshark,
    debugger,
    record_input,
    replay_input,
    tas_editor,
    sprite_editor,
    shortcuts,
    about,
    palette_first = 0x8200,
    video_first = 0x8300,
};

class DesktopMenuBar {
public:
    ~DesktopMenuBar() { detach(); }

    void attach(SDL_Window* window) {
        detach();
        hwnd_ = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd_ == nullptr) return;
        root_ = CreateMenu();
        file_ = CreatePopupMenu();
        emulation_ = CreatePopupMenu();
        view_ = CreatePopupMenu();
        palette_ = CreatePopupMenu();
        video_ = CreatePopupMenu();
        tools_ = CreatePopupMenu();
        help_ = CreatePopupMenu();
        if (root_ == nullptr || file_ == nullptr || emulation_ == nullptr ||
            view_ == nullptr || palette_ == nullptr || video_ == nullptr ||
            tools_ == nullptr || help_ == nullptr) {
            detach();
            return;
        }
        append(file_, DesktopMenuCommand::open_rom, L"&Open ROM...\tCtrl+O");
        append(file_, DesktopMenuCommand::library, L"Game &Library\tCtrl+L");
        AppendMenuW(file_, MF_SEPARATOR, 0, nullptr);
        append(file_, DesktopMenuCommand::save_state, L"&Save State");
        append(file_, DesktopMenuCommand::load_state, L"&Load State");
        AppendMenuW(file_, MF_SEPARATOR, 0, nullptr);
        append(file_, DesktopMenuCommand::exit_app, L"E&xit");

        append(emulation_, DesktopMenuCommand::pause, L"&Pause / Resume\tSpace");
        append(emulation_, DesktopMenuCommand::reset, L"&Reset\tCtrl+R");

        append(view_, DesktopMenuCommand::fullscreen, L"&Fullscreen\tF11");
        AppendMenuW(view_, MF_SEPARATOR, 0, nullptr);
        for (std::size_t index = 0; index < gameboy::display_palettes.size();
             ++index) {
            AppendMenuA(palette_, MF_STRING,
                        command_id(DesktopMenuCommand::palette_first) + index,
                        gameboy::display_palettes[index].name);
        }
        for (std::size_t index = 0; index < gameboy::video_modes.size(); ++index) {
            AppendMenuA(video_, MF_STRING,
                        command_id(DesktopMenuCommand::video_first) + index,
                        gameboy::video_modes[index].name.data());
        }
        AppendMenuW(view_, MF_POPUP, reinterpret_cast<UINT_PTR>(palette_),
                    L"&Color Palette");
        AppendMenuW(view_, MF_POPUP, reinterpret_cast<UINT_PTR>(video_),
                    L"&Video Pipeline");

        append(tools_, DesktopMenuCommand::controls,
               L"&Controls and Settings...\tCtrl+K");
        AppendMenuW(tools_, MF_SEPARATOR, 0, nullptr);
        append(tools_, DesktopMenuCommand::gameshark,
               L"&GameShark Cheats...\tCtrl+G");
        append(tools_, DesktopMenuCommand::debugger, L"&Debugger...\tF12");
        AppendMenuW(tools_, MF_SEPARATOR, 0, nullptr);
        append(tools_, DesktopMenuCommand::record_input,
               L"Record &Input...\tF6");
        append(tools_, DesktopMenuCommand::replay_input,
               L"&Replay Last Input\tF7");
        append(tools_, DesktopMenuCommand::tas_editor,
               L"&TAS Frame Editor...\tF8");
        append(tools_, DesktopMenuCommand::sprite_editor,
               L"&Sprite Editor...\tF9");

        append(help_, DesktopMenuCommand::shortcuts, L"&Shortcuts\tF1");
        append(help_, DesktopMenuCommand::about, L"&About Go Bigger Boy");

        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(file_), L"&File");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation_),
                    L"&Emulation");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(view_), L"&View");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(tools_), L"&Tools");
        AppendMenuW(root_, MF_POPUP, reinterpret_cast<UINT_PTR>(help_), L"&Help");
        SetMenu(hwnd_, root_);
        DrawMenuBar(hwnd_);
        active_ = this;
        previous_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&window_proc)));
    }

    void detach() noexcept {
        if (hwnd_ != nullptr && previous_ != nullptr) {
            SetWindowLongPtrW(hwnd_, GWLP_WNDPROC,
                              reinterpret_cast<LONG_PTR>(previous_));
        }
        if (active_ == this) active_ = nullptr;
        if (hwnd_ != nullptr && root_ != nullptr) {
            SetMenu(hwnd_, nullptr);
            DrawMenuBar(hwnd_);
        }
        if (root_ != nullptr) DestroyMenu(root_);
        hwnd_ = nullptr;
        previous_ = nullptr;
        root_ = file_ = emulation_ = view_ = palette_ = video_ = tools_ = help_ =
            nullptr;
        pending_.store(0);
    }

    [[nodiscard]] DesktopMenuCommand take_command() noexcept {
        return static_cast<DesktopMenuCommand>(pending_.exchange(0));
    }

    void update(const bool has_rom, const bool paused, const bool fullscreen,
                const bool recording, const std::size_t palette,
                const gameboy::VideoMode video) {
        if (root_ == nullptr) return;
        enable(DesktopMenuCommand::save_state, has_rom);
        enable(DesktopMenuCommand::load_state, has_rom);
        enable(DesktopMenuCommand::pause, has_rom);
        enable(DesktopMenuCommand::reset, has_rom);
        enable(DesktopMenuCommand::gameshark, has_rom);
        enable(DesktopMenuCommand::debugger, has_rom);
        enable(DesktopMenuCommand::record_input, has_rom);
        enable(DesktopMenuCommand::replay_input, has_rom);
        enable(DesktopMenuCommand::tas_editor, has_rom);
        enable(DesktopMenuCommand::sprite_editor, has_rom);
        check(DesktopMenuCommand::pause, paused);
        check(DesktopMenuCommand::fullscreen, fullscreen);
        check(DesktopMenuCommand::record_input, recording);
        for (std::size_t index = 0; index < gameboy::display_palettes.size();
             ++index) {
            CheckMenuItem(root_, command_id(DesktopMenuCommand::palette_first) +
                                     static_cast<UINT>(index),
                          MF_BYCOMMAND | (index == palette ? MF_CHECKED
                                                           : MF_UNCHECKED));
        }
        for (std::size_t index = 0; index < gameboy::video_modes.size(); ++index) {
            CheckMenuItem(root_, command_id(DesktopMenuCommand::video_first) +
                                     static_cast<UINT>(index),
                          MF_BYCOMMAND |
                              (gameboy::video_modes[index].mode == video
                                   ? MF_CHECKED
                                   : MF_UNCHECKED));
        }
    }

private:
    static UINT command_id(const DesktopMenuCommand command) noexcept {
        return static_cast<UINT>(command);
    }

    static void append(HMENU menu, const DesktopMenuCommand command,
                       const wchar_t* label) {
        AppendMenuW(menu, MF_STRING, command_id(command), label);
    }

    void enable(const DesktopMenuCommand command, const bool enabled) {
        EnableMenuItem(root_, command_id(command),
                       MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    }

    void check(const DesktopMenuCommand command, const bool checked) {
        CheckMenuItem(root_, command_id(command),
                      MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                        LPARAM lparam) {
        if (message == WM_COMMAND && active_ != nullptr &&
            HIWORD(wparam) == 0) {
            active_->pending_.store(static_cast<int>(LOWORD(wparam)));
            return 0;
        }
        return active_ != nullptr && active_->previous_ != nullptr
                   ? CallWindowProcW(active_->previous_, hwnd, message, wparam,
                                     lparam)
                   : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    inline static DesktopMenuBar* active_{};
    std::atomic_int pending_{};
    HWND hwnd_{};
    WNDPROC previous_{};
    HMENU root_{};
    HMENU file_{};
    HMENU emulation_{};
    HMENU view_{};
    HMENU palette_{};
    HMENU video_{};
    HMENU tools_{};
    HMENU help_{};
};
#endif

class SdlResources {
public:
    explicit SdlResources(const bool initially_hidden = false) {
#ifdef __ANDROID__
        if (!SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1")) {
            sdl_error("Could not trap the Android back button");
        }
#endif
        if (!SDL_SetAppMetadata("Go Bigger Boy (GBB)", GBB_VERSION,
                                "go-bigger-boy")) {
            sdl_error("Could not set application metadata");
        }
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            sdl_error("Could not initialize SDL");
        }
        auto window_flags = static_cast<SDL_WindowFlags>(SDL_WINDOW_RESIZABLE);
        if (initially_hidden) window_flags |= SDL_WINDOW_HIDDEN;
        window = SDL_CreateWindow(
            "Go Bigger Boy (GBB) - Drop a ROM here or press Ctrl+O", 640, 576,
            window_flags);
        if (window == nullptr) sdl_error("Could not create window");
        renderer = SDL_CreateRenderer(window, nullptr);
        if (renderer == nullptr) sdl_error("Could not create renderer");
        if (!SDL_SetRenderLogicalPresentation(
                renderer, static_cast<int>(gameboy::Ppu::screen_width),
                static_cast<int>(gameboy::Ppu::screen_height),
                SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
            sdl_error("Could not configure logical presentation");
        }
        texture = SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (texture == nullptr) sdl_error("Could not create framebuffer texture");
        if (!SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST)) {
            sdl_error("Could not configure nearest-neighbor scaling");
        }
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            const SDL_AudioSpec audio_spec{
                SDL_AUDIO_S16, 2, static_cast<int>(gameboy::Apu::sample_rate)};
            audio_stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
            if (audio_stream != nullptr &&
                !SDL_ResumeAudioStreamDevice(audio_stream)) {
                SDL_DestroyAudioStream(audio_stream);
                audio_stream = nullptr;
            }
        }
        if (audio_stream == nullptr) {
            std::cerr << "Warning: audio output is unavailable: "
                      << SDL_GetError() << '\n';
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer, 16, 20, 16, 255));
    }

    ~SdlResources() {
        if (camera != nullptr) SDL_CloseCamera(camera);
        if (camera_frame != nullptr) SDL_DestroySurface(camera_frame);
        if (gamepad != nullptr) {
            static_cast<void>(SDL_RumbleGamepad(gamepad, 0, 0, 0));
            SDL_CloseGamepad(gamepad);
        }
        if (audio_stream != nullptr) SDL_DestroyAudioStream(audio_stream);
        if (texture != nullptr) SDL_DestroyTexture(texture);
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    SdlResources(const SdlResources&) = delete;
    SdlResources& operator=(const SdlResources&) = delete;

    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    gbb::SceneSnapshot scene_snapshot{};
    std::filesystem::path voxel_profile_path;
    gbb::VoxelProfile voxel_profile{};
    std::uint64_t voxel_profile_fingerprint{};
    bool voxel_profile_loaded{};
    SDL_Gamepad* gamepad{};
    SDL_AudioStream* audio_stream{};
    SDL_Camera* camera{};
    SDL_Surface* camera_frame{};
    bool mirror_camera{};
    bool camera_back_facing{};
    bool camera_warning_shown{};
    std::chrono::steady_clock::time_point next_camera_frame{};
    bool rumble_output_active{};
    bool rumble_warning_shown{};
    std::chrono::steady_clock::time_point rumble_refresh{};
    std::vector<SDL_Vertex> voxel_vertices;
    std::vector<int> voxel_indices;
#ifdef __ANDROID__
    struct TouchPoint {
        SDL_FingerID id{};
        float x{};
        float y{};
    };
    std::vector<TouchPoint> touches;
    std::array<bool, 8> touch_buttons{};
    TouchControlSettings touch_settings;
#endif
};

#ifndef __ANDROID__
class InputMovie {
public:
    enum class Mode { idle, recording, replaying };
    static constexpr std::array movie_buttons{
        gameboy::Button::right, gameboy::Button::left, gameboy::Button::up,
        gameboy::Button::down, gameboy::Button::a, gameboy::Button::b,
        gameboy::Button::select, gameboy::Button::start};

    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] bool recording() const noexcept {
        return mode_ == Mode::recording;
    }
    [[nodiscard]] bool replaying() const noexcept {
        return mode_ == Mode::replaying;
    }
    [[nodiscard]] std::size_t event_count() const noexcept {
        return events_.size();
    }

    void start_recording(gameboy::Emulator& emulator) {
        for (const auto button : movie_buttons) emulator.set_button(button, false);
        pressed_.fill(false);
        start_state_ = emulator.save_state();
        fingerprint_ = emulator.rom_fingerprint();
        origin_cycles_ = emulator.cpu().total_cycles();
        events_.clear();
        next_event_ = 0;
        mode_ = Mode::recording;
    }

    void stop_and_save(const std::filesystem::path& path,
                       gameboy::Emulator& emulator) {
        if (!recording()) return;
        release_all(emulator);
        mode_ = Mode::idle;
        save_file(path);
    }

    void save_frame_inputs(gameboy::Emulator& emulator,
                           const std::filesystem::path& path,
                           const std::uint64_t fingerprint,
                           const std::vector<std::uint8_t>& start_state,
                           const std::vector<std::uint8_t>& frame_masks) {
        if (start_state.empty() || frame_masks.empty()) {
            throw std::runtime_error("The TAS timeline has no frames.");
        }
        fingerprint_ = fingerprint;
        start_state_ = start_state;
        events_.clear();
        const auto restore_state = emulator.save_state();
        try {
            emulator.load_state(start_state_);
            const auto origin = emulator.cpu().total_cycles();
            std::uint8_t previous = 0;
            for (const auto current : frame_masks) {
                const auto changed = static_cast<std::uint8_t>(previous ^ current);
                const auto elapsed = emulator.cpu().total_cycles() - origin;
                for (std::size_t button = 0; button < movie_buttons.size();
                     ++button) {
                    const auto bit = static_cast<std::uint8_t>(1U << button);
                    if ((changed & bit) == 0) continue;
                    const auto pressed = (current & bit) != 0;
                    events_.push_back({elapsed, static_cast<std::uint8_t>(button),
                                       pressed});
                    emulator.set_button(movie_buttons[button], pressed);
                }
                if (emulator.frame_ready()) emulator.consume_frame();
                unsigned cycles = 0;
                const auto lcd_enabled =
                    (emulator.bus().read8(0xFF40) & 0x80U) != 0;
                const auto disabled_lcd_budget =
                    emulator.bus().double_speed() ? 140448U : 70224U;
                const auto budget = lcd_enabled ? 280896U : disabled_lcd_budget;
                while (cycles < budget && !emulator.frame_ready()) {
                    cycles += emulator.step();
                }
                if (emulator.frame_ready()) emulator.consume_frame();
                previous = current;
            }
            const auto elapsed = emulator.cpu().total_cycles() - origin;
            for (std::size_t button = 0; button < movie_buttons.size(); ++button) {
                const auto bit = static_cast<std::uint8_t>(1U << button);
                if ((previous & bit) != 0) {
                    events_.push_back({elapsed, static_cast<std::uint8_t>(button),
                                       false});
                }
            }
            emulator.load_state(restore_state);
        } catch (...) {
            emulator.load_state(restore_state);
            throw;
        }
        mode_ = Mode::idle;
        next_event_ = 0;
        save_file(path);
    }

    void start_replay(gameboy::Emulator& emulator,
                      const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("No input recording is available yet.");
        std::array<char, 8> magic{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        constexpr std::array<char, 8> expected{'G', 'B', 'B', 'M', 'O', 'V', '1', '\0'};
        if (magic != expected) throw std::runtime_error("Invalid input recording file.");
        const auto fingerprint = read<std::uint64_t>(input);
        const auto state_size = read<std::uint32_t>(input);
        const auto event_count = read<std::uint32_t>(input);
        if (fingerprint != emulator.rom_fingerprint()) {
            throw std::runtime_error("This recording belongs to a different ROM.");
        }
        if (state_size == 0 || state_size > 2U * 1024U * 1024U ||
            event_count > 1000000U) {
            throw std::runtime_error("Input recording is unreasonably large.");
        }
        start_state_.resize(state_size);
        input.read(reinterpret_cast<char*>(start_state_.data()),
                   static_cast<std::streamsize>(start_state_.size()));
        events_.clear();
        events_.reserve(event_count);
        for (std::uint32_t index = 0; index < event_count; ++index) {
            Event event;
            event.cycle = read<std::uint64_t>(input);
            event.button = read<std::uint8_t>(input);
            event.pressed = read<std::uint8_t>(input) != 0;
            if (event.button >= movie_buttons.size() ||
                (!events_.empty() && event.cycle < events_.back().cycle)) {
                throw std::runtime_error("Input recording contains invalid events.");
            }
            events_.push_back(event);
        }
        if (!input) throw std::runtime_error("Input recording is truncated.");
        emulator.load_state(start_state_);
        pressed_.fill(false);
        fingerprint_ = fingerprint;
        origin_cycles_ = emulator.cpu().total_cycles();
        next_event_ = 0;
        mode_ = Mode::replaying;
    }

    void stop(gameboy::Emulator* emulator = nullptr) noexcept {
        if (emulator != nullptr) {
            for (const auto button : movie_buttons) emulator->set_button(button, false);
        }
        pressed_.fill(false);
        mode_ = Mode::idle;
        next_event_ = 0;
    }

    void set_button(gameboy::Emulator& emulator, const gameboy::Button button,
                    const bool pressed) {
        if (replaying()) return;
        const auto found = std::find(movie_buttons.begin(), movie_buttons.end(), button);
        if (found == movie_buttons.end()) return;
        const auto index = static_cast<std::size_t>(found - movie_buttons.begin());
        if (pressed_[index] == pressed) return;
        pressed_[index] = pressed;
        emulator.set_button(button, pressed);
        if (recording()) {
            events_.push_back({emulator.cpu().total_cycles() - origin_cycles_,
                               static_cast<std::uint8_t>(index), pressed});
        }
    }

    void release_all(gameboy::Emulator& emulator) {
        for (const auto button : movie_buttons) set_button(emulator, button, false);
    }

    [[nodiscard]] bool update_replay(gameboy::Emulator& emulator) {
        if (!replaying()) return false;
        const auto elapsed = emulator.cpu().total_cycles() - origin_cycles_;
        while (next_event_ < events_.size() &&
               events_[next_event_].cycle <= elapsed) {
            const auto& event = events_[next_event_++];
            pressed_[event.button] = event.pressed;
            emulator.set_button(movie_buttons[event.button], event.pressed);
        }
        if (next_event_ == events_.size()) {
            stop(&emulator);
            return true;
        }
        return false;
    }

private:
    struct Event {
        std::uint64_t cycle{};
        std::uint8_t button{};
        bool pressed{};
    };

    template <typename Value>
    static void write(std::ostream& output, const Value value) {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    template <typename Value>
    static Value read(std::istream& input) {
        Value value{};
        input.read(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    }

    void save_file(const std::filesystem::path& path) const {
        if (path.empty()) return;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not save input recording.");
        constexpr std::array<char, 8> magic{'G', 'B', 'B', 'M', 'O', 'V', '1', '\0'};
        output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        write(output, fingerprint_);
        write(output, static_cast<std::uint32_t>(start_state_.size()));
        write(output, static_cast<std::uint32_t>(events_.size()));
        output.write(reinterpret_cast<const char*>(start_state_.data()),
                     static_cast<std::streamsize>(start_state_.size()));
        for (const auto& event : events_) {
            write(output, event.cycle);
            write(output, event.button);
            write(output, static_cast<std::uint8_t>(event.pressed ? 1 : 0));
        }
        if (!output) throw std::runtime_error("Could not finish input recording.");
    }

    Mode mode_{Mode::idle};
    std::uint64_t fingerprint_{};
    std::uint64_t origin_cycles_{};
    std::vector<std::uint8_t> start_state_;
    std::vector<Event> events_;
    std::array<bool, movie_buttons.size()> pressed_{};
    std::size_t next_event_{};
};

class TasEditor {
public:
    ~TasEditor() { close(); }
    TasEditor() = default;
    TasEditor(const TasEditor&) = delete;
    TasEditor& operator=(const TasEditor&) = delete;

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool take_save_request() noexcept {
        return std::exchange(save_requested_, false);
    }
    [[nodiscard]] bool take_replay_request() noexcept {
        return std::exchange(replay_requested_, false);
    }
    [[nodiscard]] bool take_new_request() noexcept {
        return std::exchange(new_requested_, false);
    }
    [[nodiscard]] const std::vector<std::uint8_t>& frames() const noexcept {
        return frames_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& start_state() const noexcept {
        return start_state_;
    }
    [[nodiscard]] std::uint64_t fingerprint() const noexcept {
        return fingerprint_;
    }

    void open(SDL_Window* parent, gameboy::Emulator& emulator) {
        if (!visible()) {
            window_ = SDL_CreateWindow("Go Bigger Boy - TAS Input Editor",
                                       920, 700, SDL_WINDOW_RESIZABLE);
            if (window_ == nullptr) sdl_error("Could not create TAS editor window");
            static_cast<void>(SDL_SetWindowMinimumSize(window_, 900, 520));
            renderer_ = SDL_CreateRenderer(window_, nullptr);
            if (renderer_ == nullptr) {
                close();
                sdl_error("Could not create TAS editor renderer");
            }
            if (parent != nullptr) {
                int x = 0;
                int y = 0;
                static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
                static_cast<void>(SDL_SetWindowPosition(window_, x + 80, y + 80));
            }
        }
        if (start_state_.empty() || fingerprint_ != emulator.rom_fingerprint()) {
            reset_from(emulator);
        }
        SDL_RaiseWindow(window_);
    }

    void reset_from(gameboy::Emulator& emulator) {
        for (const auto button : InputMovie::movie_buttons) {
            emulator.set_button(button, false);
        }
        start_state_ = emulator.save_state();
        fingerprint_ = emulator.rom_fingerprint();
        frames_.assign(1, 0);
        selection_ = 0;
        first_visible_ = 0;
    }

    void close() noexcept {
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
        save_requested_ = false;
        replay_requested_ = false;
        new_requested_ = false;
    }

    bool handle_event(const SDL_Event& event) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        SDL_WindowID event_window = 0;
        if (event.type >= SDL_EVENT_WINDOW_FIRST &&
            event.type <= SDL_EVENT_WINDOW_LAST) {
            event_window = event.window.windowID;
        } else if (event.type == SDL_EVENT_KEY_DOWN ||
                   event.type == SDL_EVENT_KEY_UP) {
            event_window = event.key.windowID;
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                   event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            event_window = event.button.windowID;
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            event_window = event.wheel.windowID;
        }
        if (event_window != id) return false;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close();
            return true;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.key == SDLK_ESCAPE) {
                close();
            } else if (event.key.key == SDLK_UP && selection_ > 0) {
                --selection_;
                keep_selection_visible();
            } else if (event.key.key == SDLK_DOWN) {
                if (selection_ + 1 < frames_.size()) ++selection_;
                keep_selection_visible();
            } else if (event.key.key == SDLK_INSERT) {
                frames_.insert(frames_.begin() + static_cast<std::ptrdiff_t>(selection_),
                               0);
            } else if (event.key.key == SDLK_DELETE) {
                delete_selected();
            } else if (event.key.key == SDLK_END) {
                frames_.push_back(0);
                selection_ = frames_.size() - 1;
                keep_selection_visible();
            } else if (event.key.key == SDLK_N &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                new_requested_ = true;
            } else if (event.key.key == SDLK_S &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                save_requested_ = true;
            } else if (event.key.key == SDLK_F7) {
                replay_requested_ = true;
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (event.wheel.y > 0 && first_visible_ > 0) --first_visible_;
            if (event.wheel.y < 0 && first_visible_ + 1 < frames_.size()) {
                ++first_visible_;
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            constexpr float first_row_y = 94.0F;
            constexpr float row_height = 22.0F;
            const auto bottom_y = static_cast<float>(height - 55);
            if (event.button.y >= first_row_y && event.button.y < bottom_y - 18) {
                const auto row = static_cast<std::size_t>(
                    (event.button.y - first_row_y) / row_height);
                const auto frame = first_visible_ + row;
                if (frame < frames_.size()) {
                    selection_ = frame;
                    constexpr float first_button_x = 144.0F;
                    constexpr float column_width = 82.0F;
                    if (event.button.x >= first_button_x) {
                        const auto button = static_cast<std::size_t>(
                            (event.button.x - first_button_x) / column_width);
                        if (button < InputMovie::movie_buttons.size()) {
                            frames_[frame] ^= static_cast<std::uint8_t>(1U << button);
                        }
                    }
                }
            } else if (event.button.y >= bottom_y &&
                       event.button.y <= bottom_y + 36.0F) {
                const auto x = event.button.x;
                if (x >= 24 && x <= 154) {
                    frames_.insert(
                        frames_.begin() + static_cast<std::ptrdiff_t>(selection_), 0);
                } else if (x >= 166 && x <= 296) {
                    delete_selected();
                } else if (x >= 308 && x <= 438) {
                    frames_.push_back(0);
                    selection_ = frames_.size() - 1;
                    keep_selection_visible();
                } else if (x >= 450 && x <= 580) {
                    save_requested_ = true;
                } else if (x >= 592 && x <= 722) {
                    replay_requested_ = true;
                } else if (x >= 734 && x <= 864) {
                    new_requested_ = true;
                }
            }
            return true;
        }
        return true;
    }

    void present() {
        if (!visible()) return;
        int width = 0;
        int height = 0;
        static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, 24, 18,
                                              "TAS FRAME INPUT EDITOR"));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 38,
            "CLICK CELLS TO HOLD BUTTONS FOR A FRAME  |  UP/DOWN SELECT"));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 54,
            "INSERT ADD BEFORE  DELETE REMOVE  END APPEND  CTRL+S SAVE  F7 RUN"));

        constexpr std::array<const char*, 8> names{
            "RIGHT", "LEFT", "UP", "DOWN", "A", "B", "SELECT", "START"};
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, 28, 76, "FRAME"));
        constexpr float first_button_x = 144.0F;
        constexpr float column_width = 82.0F;
        for (std::size_t button = 0; button < names.size(); ++button) {
            static_cast<void>(SDL_RenderDebugText(
                renderer_, first_button_x + button * column_width + 8, 76,
                names[button]));
        }
        constexpr float first_row_y = 94.0F;
        constexpr float row_height = 22.0F;
        const auto visible_rows = std::max(
            1, static_cast<int>((height - 185.0F) / row_height));
        if (selection_ < first_visible_) first_visible_ = selection_;
        if (selection_ >= first_visible_ + static_cast<std::size_t>(visible_rows)) {
            first_visible_ = selection_ - static_cast<std::size_t>(visible_rows) + 1;
        }
        for (int row = 0; row < visible_rows; ++row) {
            const auto frame = first_visible_ + static_cast<std::size_t>(row);
            if (frame >= frames_.size()) break;
            const auto y = first_row_y + row * row_height;
            const SDL_FRect background{24, y - 3, 840, row_height - 2};
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer_, frame == selection_ ? 20 : 16,
                frame == selection_ ? 77 : 27,
                frame == selection_ ? 101 : 39, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &background));
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
            const auto frame_text = std::to_string(frame);
            static_cast<void>(SDL_RenderDebugText(renderer_, 30, y + 3,
                                                  frame_text.c_str()));
            for (std::size_t button = 0; button < names.size(); ++button) {
                const auto active = (frames_[frame] & (1U << button)) != 0;
                const SDL_FRect cell{first_button_x + button * column_width,
                                     y - 2, column_width - 6, row_height - 4};
                static_cast<void>(SDL_SetRenderDrawColor(
                    renderer_, active ? 69 : 34, active ? 207 : 54,
                    active ? 238 : 70, 255));
                if (active) static_cast<void>(SDL_RenderFillRect(renderer_, &cell));
                static_cast<void>(SDL_RenderRect(renderer_, &cell));
                if (active) {
                    static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
                    static_cast<void>(SDL_RenderDebugText(renderer_, cell.x + 30,
                                                          y + 3, "X"));
                }
            }
        }
        const auto button = [this](const SDL_FRect& rect, const char* label) {
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 28, 47, 68, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &rect));
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
            static_cast<void>(SDL_RenderRect(renderer_, &rect));
            static_cast<void>(SDL_RenderDebugText(renderer_, rect.x + 10,
                                                  rect.y + 14, label));
        };
        const auto bottom_y = static_cast<float>(height - 55);
        button({24, bottom_y, 130, 36}, "INSERT FRAME");
        button({166, bottom_y, 130, 36}, "DELETE FRAME");
        button({308, bottom_y, 130, 36}, "APPEND FRAME");
        button({450, bottom_y, 130, 36}, "SAVE MOVIE");
        button({592, bottom_y, 130, 36}, "RUN MOVIE");
        button({734, bottom_y, 130, 36}, "NEW FROM NOW");
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

private:
    void delete_selected() {
        if (frames_.size() > 1) {
            frames_.erase(frames_.begin() +
                          static_cast<std::ptrdiff_t>(selection_));
            if (selection_ >= frames_.size()) selection_ = frames_.size() - 1;
            keep_selection_visible();
        } else {
            frames_[0] = 0;
        }
    }

    void keep_selection_visible() noexcept {
        if (selection_ < first_visible_) first_visible_ = selection_;
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    std::uint64_t fingerprint_{};
    std::vector<std::uint8_t> start_state_;
    std::vector<std::uint8_t> frames_{1, 0};
    std::size_t selection_{};
    std::size_t first_visible_{};
    bool save_requested_{};
    bool replay_requested_{};
    bool new_requested_{};
};

class SpriteEditor {
public:
    struct IpsExportResult {
        std::size_t exported{};
        std::size_t unresolved{};
    };

    SpriteEditor() = default;
    ~SpriteEditor() { close(); }
    SpriteEditor(const SpriteEditor&) = delete;
    SpriteEditor& operator=(const SpriteEditor&) = delete;

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool take_save_patch_request() noexcept {
        return std::exchange(save_patch_requested_, false);
    }
    [[nodiscard]] bool take_load_patch_request() noexcept {
        return std::exchange(load_patch_requested_, false);
    }
    [[nodiscard]] bool take_export_ips_request() noexcept {
        return std::exchange(export_ips_requested_, false);
    }

    void open(SDL_Window* parent, const gameboy::Emulator& emulator) {
        if (visible()) {
            if (baseline_.empty() || fingerprint_ != emulator.rom_fingerprint()) {
                capture_baseline(emulator);
            }
            SDL_RaiseWindow(window_);
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - Live Sprite Editor",
                                   1000, 780, SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) sdl_error("Could not create sprite editor window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 960, 760));
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            close();
            sdl_error("Could not create sprite editor renderer");
        }
        if (parent != nullptr) {
            int x = 0;
            int y = 0;
            static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
            static_cast<void>(SDL_SetWindowPosition(window_, x + 110, y + 110));
        }
        if (baseline_.empty() || fingerprint_ != emulator.rom_fingerprint()) {
            capture_baseline(emulator);
        }
    }

    void close() noexcept {
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
        painting_ = false;
        have_undo_ = false;
        save_patch_requested_ = false;
        load_patch_requested_ = false;
        export_ips_requested_ = false;
    }

    void reset_session() noexcept {
        close();
        baseline_.clear();
        fingerprint_ = 0;
    }

    bool handle_event(const SDL_Event& event, gameboy::Emulator* emulator) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        SDL_WindowID event_window = 0;
        if (event.type >= SDL_EVENT_WINDOW_FIRST &&
            event.type <= SDL_EVENT_WINDOW_LAST) {
            event_window = event.window.windowID;
        } else if (event.type == SDL_EVENT_KEY_DOWN ||
                   event.type == SDL_EVENT_KEY_UP) {
            event_window = event.key.windowID;
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                   event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            event_window = event.button.windowID;
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            event_window = event.motion.windowID;
        }
        if (event_window != id) return false;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close();
            return true;
        }
        if (emulator == nullptr) return true;
        if (!emulator->bus().cgb_mode()) bank_ = 0;
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_F9) {
                close();
            } else if (event.key.key >= SDLK_1 && event.key.key <= SDLK_4) {
                color_ = static_cast<std::uint8_t>(event.key.key - SDLK_1);
            } else if (event.key.key == SDLK_B && emulator->bus().cgb_mode()) {
                bank_ ^= 1U;
                have_undo_ = false;
            } else if (event.key.key == SDLK_DELETE) {
                snapshot(*emulator);
                clear_tile(*emulator);
            } else if (event.key.key == SDLK_Z &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                undo(*emulator);
            } else if (event.key.key == SDLK_S &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                save_patch_requested_ = true;
            } else if (event.key.key == SDLK_O &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                load_patch_requested_ = true;
            } else if (event.key.key == SDLK_E &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                export_ips_requested_ = true;
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (select_tile_at(event.button.x, event.button.y)) {
                have_undo_ = false;
                return true;
            }
            if (select_color_at(event.button.x, event.button.y)) return true;
            if (handle_button(event.button.x, event.button.y, *emulator)) {
                return true;
            }
            if (paint_position(event.button.x, event.button.y)) {
                snapshot(*emulator);
                painting_ = true;
                paint(*emulator, event.button.x, event.button.y,
                      event.button.button == SDL_BUTTON_RIGHT ? 0 : color_);
            }
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && painting_) {
            const auto right = (event.motion.state & SDL_BUTTON_RMASK) != 0;
            paint(*emulator, event.motion.x, event.motion.y,
                  right ? 0 : color_);
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            painting_ = false;
            return true;
        }
        return true;
    }

    void present(const gameboy::Emulator* emulator) {
        if (!visible() || emulator == nullptr) return;
        if (!emulator->bus().cgb_mode()) bank_ = 0;
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, 24, 18,
                                              "LIVE VRAM SPRITE / TILE EDITOR"));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 38,
            "SELECT A TILE, THEN PAINT ITS 2-BIT COLOR INDICES"));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 54,
            "CHANGES ARE LIVE AND MAY BE OVERWRITTEN WHEN THE GAME RESUMES"));

        constexpr float grid_x = 24.0F;
        constexpr float grid_y = 82.0F;
        constexpr float tile_size = 24.0F;
        constexpr std::size_t columns = 16;
        for (std::size_t tile = 0; tile < 384; ++tile) {
            const auto tile_x = grid_x + (tile % columns) * tile_size;
            const auto tile_y = grid_y + (tile / columns) * tile_size;
            for (std::size_t y = 0; y < 8; ++y) {
                const auto low = emulator->bus().debug_read_vram(
                    bank_, static_cast<std::uint16_t>(tile * 16 + y * 2));
                const auto high = emulator->bus().debug_read_vram(
                    bank_, static_cast<std::uint16_t>(tile * 16 + y * 2 + 1));
                for (std::size_t x = 0; x < 8; ++x) {
                    const auto shift = 7U - x;
                    const auto color = static_cast<std::uint8_t>(
                        ((high >> shift) & 1U) << 1U | ((low >> shift) & 1U));
                    set_color(color);
                    const SDL_FRect pixel{tile_x + x * 3.0F, tile_y + y * 3.0F,
                                          3, 3};
                    static_cast<void>(SDL_RenderFillRect(renderer_, &pixel));
                }
            }
            if (tile == selected_tile_) {
                static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
                const SDL_FRect selection{tile_x - 2, tile_y - 2,
                                          tile_size + 4, tile_size + 4};
                static_cast<void>(SDL_RenderRect(renderer_, &selection));
            }
        }

        constexpr float editor_x = 500.0F;
        constexpr float editor_y = 100.0F;
        constexpr float pixel_size = 48.0F;
        for (std::size_t y = 0; y < 8; ++y) {
            const auto low = emulator->bus().debug_read_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + y * 2));
            const auto high = emulator->bus().debug_read_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + y * 2 + 1));
            for (std::size_t x = 0; x < 8; ++x) {
                const auto shift = 7U - x;
                const auto color = static_cast<std::uint8_t>(
                    ((high >> shift) & 1U) << 1U | ((low >> shift) & 1U));
                set_color(color);
                const SDL_FRect pixel{editor_x + x * pixel_size,
                                      editor_y + y * pixel_size,
                                      pixel_size, pixel_size};
                static_cast<void>(SDL_RenderFillRect(renderer_, &pixel));
                static_cast<void>(SDL_SetRenderDrawColor(renderer_, 28, 47, 68, 255));
                static_cast<void>(SDL_RenderRect(renderer_, &pixel));
            }
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
        const auto tile_label = "TILE " + std::to_string(selected_tile_) +
                                "  VRAM $" + hex_address() +
                                "  BANK " + std::to_string(bank_);
        static_cast<void>(SDL_RenderDebugText(renderer_, editor_x, 82,
                                              tile_label.c_str()));
        for (std::uint8_t color = 0; color < 4; ++color) {
            const SDL_FRect swatch{editor_x + color * 80.0F, 520, 56, 56};
            set_color(color);
            static_cast<void>(SDL_RenderFillRect(renderer_, &swatch));
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer_, color == color_ ? 69 : 52,
                color == color_ ? 207 : 75,
                color == color_ ? 238 : 91, 255));
            static_cast<void>(SDL_RenderRect(renderer_, &swatch));
            const auto label = std::to_string(color + 1);
            static_cast<void>(SDL_RenderDebugText(renderer_, swatch.x + 24,
                                                  swatch.y + 62, label.c_str()));
        }
        draw_button({500, 620, 120, 36}, "CTRL+Z UNDO");
        draw_button({636, 620, 120, 36}, "DELETE CLEAR");
        draw_button({772, 620, 120, 36},
                    emulator->bus().cgb_mode()
                        ? (bank_ == 0 ? "B  BANK 0" : "B  BANK 1")
                        : "DMG BANK 0");
        draw_button({500, 670, 120, 36}, "CTRL+S PATCH");
        draw_button({636, 670, 120, 36}, "CTRL+O IMPORT");
        draw_button({772, 670, 120, 36}, "CTRL+E IPS");
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

    void save_patch(const gameboy::Emulator& emulator,
                    const std::filesystem::path& path) const {
        const auto changed = changes(emulator);
        if (changed.empty()) {
            throw std::runtime_error("No sprite changes are available to save.");
        }
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not create sprite patch.");
        constexpr std::array<char, 8> magic{'G', 'B', 'B', 'T', 'I', 'L', 'E', '1'};
        output.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        write_little(output, fingerprint_, 8);
        write_little(output, changed.size(), 4);
        for (const auto& tile : changed) {
            output.put(static_cast<char>(tile.bank));
            write_little(output, tile.index, 2);
            output.write(reinterpret_cast<const char*>(tile.original.data()), 16);
            output.write(reinterpret_cast<const char*>(tile.replacement.data()), 16);
        }
        if (!output) throw std::runtime_error("Could not finish sprite patch.");
    }

    void load_patch(gameboy::Emulator& emulator,
                    const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("No sprite patch is available yet.");
        std::array<char, 8> magic{};
        input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
        constexpr std::array<char, 8> expected{'G', 'B', 'B', 'T', 'I', 'L', 'E', '1'};
        if (magic != expected) throw std::runtime_error("Invalid sprite patch file.");
        const auto fingerprint = read_little(input, 8);
        const auto count = read_little(input, 4);
        if (fingerprint != emulator.rom_fingerprint()) {
            throw std::runtime_error("This sprite patch belongs to a different ROM.");
        }
        if (count == 0 || count > 768) {
            throw std::runtime_error("Sprite patch has an invalid tile count.");
        }
        for (std::uint64_t record = 0; record < count; ++record) {
            const auto bank = static_cast<std::uint8_t>(input.get());
            const auto tile = read_little(input, 2);
            std::array<std::uint8_t, 16> original{};
            std::array<std::uint8_t, 16> replacement{};
            input.read(reinterpret_cast<char*>(original.data()), 16);
            input.read(reinterpret_cast<char*>(replacement.data()), 16);
            if (!input || bank > 1 || tile >= 384 ||
                (bank != 0 && !emulator.bus().cgb_mode())) {
                throw std::runtime_error("Sprite patch contains an invalid tile.");
            }
            for (std::size_t index = 0; index < replacement.size(); ++index) {
                emulator.bus().debug_write_vram(
                    bank, static_cast<std::uint16_t>(tile * 16 + index),
                    replacement[index]);
            }
        }
    }

    [[nodiscard]] IpsExportResult export_ips(
        const gameboy::Emulator& emulator, const std::filesystem::path& rom_path,
        const std::filesystem::path& output_path) const {
        std::ifstream rom_input(rom_path, std::ios::binary);
        if (!rom_input) throw std::runtime_error("Could not read the current ROM.");
        std::vector<std::uint8_t> rom{
            std::istreambuf_iterator<char>(rom_input),
            std::istreambuf_iterator<char>()};
        struct MappedTile {
            std::size_t offset{};
            std::array<std::uint8_t, 16> replacement{};
        };
        std::vector<MappedTile> mapped;
        IpsExportResult result;
        for (const auto& tile : changes(emulator)) {
            std::optional<std::size_t> match;
            for (std::size_t offset = 0; offset + tile.original.size() <= rom.size();
                 ++offset) {
                if (!std::equal(tile.original.begin(), tile.original.end(),
                                rom.begin() + static_cast<std::ptrdiff_t>(offset))) {
                    continue;
                }
                if (match) {
                    match.reset();
                    break;
                }
                match = offset;
            }
            if (!match || *match > 0xFFFFFFU ||
                (*match < 0x150U && *match + tile.original.size() > 0x14DU) ||
                std::any_of(mapped.begin(), mapped.end(),
                            [&](const MappedTile& existing) {
                                return existing.offset == *match;
                            })) {
                ++result.unresolved;
                continue;
            }
            mapped.push_back({*match, tile.replacement});
            ++result.exported;
        }
        if (mapped.empty()) {
            throw std::runtime_error(
                "None of the edited tiles map uniquely to uncompressed ROM data.");
        }
        auto patched_rom = rom;
        for (const auto& tile : mapped) {
            std::copy(tile.replacement.begin(), tile.replacement.end(),
                      patched_rom.begin() +
                          static_cast<std::ptrdiff_t>(tile.offset));
        }
        std::array<std::uint8_t, 3> checksums{};
        if (patched_rom.size() > 0x14FU) {
            std::uint8_t header = 0;
            for (std::size_t offset = 0x134; offset <= 0x14C; ++offset) {
                header = static_cast<std::uint8_t>(
                    header - patched_rom[offset] - 1U);
            }
            patched_rom[0x14D] = header;
            std::uint16_t global = 0;
            for (std::size_t offset = 0; offset < patched_rom.size(); ++offset) {
                if (offset == 0x14E || offset == 0x14F) continue;
                global = static_cast<std::uint16_t>(global + patched_rom[offset]);
            }
            checksums = {header, static_cast<std::uint8_t>(global >> 8U),
                         static_cast<std::uint8_t>(global & 0xFFU)};
        }
        std::filesystem::create_directories(output_path.parent_path());
        std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not create IPS patch.");
        output.write("PATCH", 5);
        for (const auto& tile : mapped) {
            write_big(output, tile.offset, 3);
            write_big(output, tile.replacement.size(), 2);
            output.write(reinterpret_cast<const char*>(tile.replacement.data()),
                         static_cast<std::streamsize>(tile.replacement.size()));
        }
        if (patched_rom.size() > 0x14FU) {
            write_big(output, 0x14DU, 3);
            write_big(output, checksums.size(), 2);
            output.write(reinterpret_cast<const char*>(checksums.data()),
                         static_cast<std::streamsize>(checksums.size()));
        }
        output.write("EOF", 3);
        if (!output) throw std::runtime_error("Could not finish IPS patch.");
        return result;
    }

private:
    struct TileChange {
        std::uint8_t bank{};
        std::uint16_t index{};
        std::array<std::uint8_t, 16> original{};
        std::array<std::uint8_t, 16> replacement{};
    };

    static constexpr float grid_x = 24.0F;
    static constexpr float grid_y = 82.0F;
    static constexpr float tile_size = 24.0F;
    static constexpr float editor_x = 500.0F;
    static constexpr float editor_y = 100.0F;
    static constexpr float pixel_size = 48.0F;

    [[nodiscard]] bool select_tile_at(const float x, const float y) {
        if (x < grid_x || x >= grid_x + 16 * tile_size || y < grid_y ||
            y >= grid_y + 24 * tile_size) {
            return false;
        }
        const auto column = static_cast<std::size_t>((x - grid_x) / tile_size);
        const auto row = static_cast<std::size_t>((y - grid_y) / tile_size);
        selected_tile_ = row * 16 + column;
        return true;
    }

    [[nodiscard]] bool select_color_at(const float x, const float y) {
        if (y < 520 || y >= 576 || x < editor_x || x >= editor_x + 320) {
            return false;
        }
        const auto color = static_cast<std::size_t>((x - editor_x) / 80);
        if (color >= 4) return false;
        color_ = static_cast<std::uint8_t>(color);
        return true;
    }

    [[nodiscard]] static bool paint_position(const float x, const float y) {
        return x >= editor_x && x < editor_x + 8 * pixel_size &&
               y >= editor_y && y < editor_y + 8 * pixel_size;
    }

    void paint(gameboy::Emulator& emulator, const float mouse_x,
               const float mouse_y, const std::uint8_t color) {
        if (!paint_position(mouse_x, mouse_y)) return;
        const auto x = static_cast<unsigned>((mouse_x - editor_x) / pixel_size);
        const auto y = static_cast<unsigned>((mouse_y - editor_y) / pixel_size);
        const auto offset = static_cast<std::uint16_t>(selected_tile_ * 16 + y * 2);
        auto low = emulator.bus().debug_read_vram(bank_, offset);
        auto high = emulator.bus().debug_read_vram(bank_, offset + 1);
        const auto mask = static_cast<std::uint8_t>(1U << (7U - x));
        low = static_cast<std::uint8_t>((low & ~mask) |
                                       ((color & 1U) != 0 ? mask : 0));
        high = static_cast<std::uint8_t>((high & ~mask) |
                                        ((color & 2U) != 0 ? mask : 0));
        emulator.bus().debug_write_vram(bank_, offset, low);
        emulator.bus().debug_write_vram(bank_, offset + 1, high);
    }

    void snapshot(const gameboy::Emulator& emulator) {
        for (std::size_t index = 0; index < undo_tile_.size(); ++index) {
            undo_tile_[index] = emulator.bus().debug_read_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + index));
        }
        undo_index_ = selected_tile_;
        undo_bank_ = bank_;
        have_undo_ = true;
    }

    void undo(gameboy::Emulator& emulator) {
        if (!have_undo_) return;
        for (std::size_t index = 0; index < undo_tile_.size(); ++index) {
            emulator.bus().debug_write_vram(
                undo_bank_, static_cast<std::uint16_t>(undo_index_ * 16 + index),
                undo_tile_[index]);
        }
        selected_tile_ = undo_index_;
        bank_ = undo_bank_;
        have_undo_ = false;
    }

    void clear_tile(gameboy::Emulator& emulator) {
        for (std::size_t index = 0; index < 16; ++index) {
            emulator.bus().debug_write_vram(
                bank_, static_cast<std::uint16_t>(selected_tile_ * 16 + index), 0);
        }
    }

    [[nodiscard]] bool handle_button(const float x, const float y,
                                     gameboy::Emulator& emulator) {
        if (y >= 670 && y <= 706) {
            if (x >= 500 && x <= 620) save_patch_requested_ = true;
            else if (x >= 636 && x <= 756) load_patch_requested_ = true;
            else if (x >= 772 && x <= 892) export_ips_requested_ = true;
            else return false;
            return true;
        }
        if (y < 620 || y > 656) return false;
        if (x >= 500 && x <= 620) {
            undo(emulator);
            return true;
        }
        if (x >= 636 && x <= 756) {
            snapshot(emulator);
            clear_tile(emulator);
            return true;
        }
        if (x >= 772 && x <= 892 && emulator.bus().cgb_mode()) {
            bank_ ^= 1U;
            have_undo_ = false;
            return true;
        }
        return false;
    }

    void capture_baseline(const gameboy::Emulator& emulator) {
        const auto banks = emulator.bus().cgb_mode() ? 2U : 1U;
        baseline_.resize(banks * 0x1800U);
        for (std::size_t bank = 0; bank < banks; ++bank) {
            for (std::size_t offset = 0; offset < 0x1800; ++offset) {
                baseline_[bank * 0x1800U + offset] =
                    emulator.bus().debug_read_vram(
                        static_cast<std::uint8_t>(bank),
                        static_cast<std::uint16_t>(offset));
            }
        }
        fingerprint_ = emulator.rom_fingerprint();
        have_undo_ = false;
    }

    [[nodiscard]] std::vector<TileChange> changes(
        const gameboy::Emulator& emulator) const {
        std::vector<TileChange> changed;
        const auto banks = emulator.bus().cgb_mode() ? 2U : 1U;
        if (baseline_.size() != banks * 0x1800U ||
            fingerprint_ != emulator.rom_fingerprint()) {
            return changed;
        }
        for (std::size_t bank = 0; bank < banks; ++bank) {
            for (std::size_t tile = 0; tile < 384; ++tile) {
                TileChange change;
                change.bank = static_cast<std::uint8_t>(bank);
                change.index = static_cast<std::uint16_t>(tile);
                for (std::size_t byte = 0; byte < 16; ++byte) {
                    change.original[byte] =
                        baseline_[bank * 0x1800U + tile * 16 + byte];
                    change.replacement[byte] = emulator.bus().debug_read_vram(
                        static_cast<std::uint8_t>(bank),
                        static_cast<std::uint16_t>(tile * 16 + byte));
                }
                if (change.original != change.replacement) {
                    changed.push_back(change);
                }
            }
        }
        return changed;
    }

    static void write_little(std::ostream& output, std::uint64_t value,
                             const unsigned bytes) {
        for (unsigned index = 0; index < bytes; ++index) {
            output.put(static_cast<char>((value >> (index * 8U)) & 0xFFU));
        }
    }

    [[nodiscard]] static std::uint64_t read_little(std::istream& input,
                                                   const unsigned bytes) {
        std::uint64_t value = 0;
        for (unsigned index = 0; index < bytes; ++index) {
            const auto byte = input.get();
            if (byte == std::char_traits<char>::eof()) return 0;
            value |= static_cast<std::uint64_t>(
                         static_cast<std::uint8_t>(byte))
                     << (index * 8U);
        }
        return value;
    }

    static void write_big(std::ostream& output, const std::uint64_t value,
                          const unsigned bytes) {
        for (unsigned index = 0; index < bytes; ++index) {
            const auto shift = (bytes - index - 1U) * 8U;
            output.put(static_cast<char>((value >> shift) & 0xFFU));
        }
    }

    void set_color(const std::uint8_t color) {
        constexpr std::array<std::array<std::uint8_t, 3>, 4> colors{{
            {{238, 249, 255}}, {{156, 192, 207}},
            {{69, 112, 133}}, {{8, 12, 20}},
        }};
        const auto& rgb = colors[color & 3U];
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, rgb[0], rgb[1],
                                                 rgb[2], 255));
    }

    void draw_button(const SDL_FRect& rect, const char* label) {
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 28, 47, 68, 255));
        static_cast<void>(SDL_RenderFillRect(renderer_, &rect));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &rect));
        static_cast<void>(SDL_RenderDebugText(renderer_, rect.x + 9,
                                              rect.y + 14, label));
    }

    [[nodiscard]] std::string hex_address() const {
        std::ostringstream output;
        output << std::uppercase << std::hex << std::setfill('0') << std::setw(4)
               << (0x8000U + selected_tile_ * 16U);
        return output.str();
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    std::size_t selected_tile_{};
    std::uint8_t bank_{};
    std::uint8_t color_{};
    std::array<std::uint8_t, 16> undo_tile_{};
    std::size_t undo_index_{};
    std::uint8_t undo_bank_{};
    bool have_undo_{};
    bool painting_{};
    std::uint64_t fingerprint_{};
    std::vector<std::uint8_t> baseline_;
    bool save_patch_requested_{};
    bool load_patch_requested_{};
    bool export_ips_requested_{};
};

class CheatManager {
public:
    ~CheatManager() { close(); }
    CheatManager(const CheatManager&) = delete;
    CheatManager& operator=(const CheatManager&) = delete;
    CheatManager() = default;

    void load(const std::filesystem::path& preference_directory,
              const gameboy::RomMetadata& metadata) {
        close();
        metadata_ = metadata;
        preference_directory_ = preference_directory;
        directory_ = preference_directory / "cheats";
        std::ostringstream name;
        name << std::hex << std::setfill('0') << std::setw(16)
             << metadata.fingerprint;
        path_ = directory_ / (name.str() + ".cht");
        cheats_.clear();
        archive_attempted_ = false;
        std::ifstream input(path_, std::ios::binary);
        if (input) {
            const std::string text{std::istreambuf_iterator<char>{input}, {}};
            cheats_ = gameboy::parse_libretro_cheats(text, false);
        }
        selected_ = cheats_.empty() ? 0 : std::min(selected_, cheats_.size() - 1);
        scroll_ = 0;
        status_ = cheats_.empty() ? "No cheats loaded. Fetch or add one manually."
                                  : std::to_string(cheats_.size()) +
                                        " cheats loaded for this ROM.";
    }

    void apply(gameboy::Emulator& emulator) const {
        gameboy::apply_gameshark_cheats(cheats_, emulator.bus());
    }

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool take_fetch_request() noexcept {
        return std::exchange(fetch_requested_, false);
    }

    void open(SDL_Window* parent) {
        if (visible()) {
            static_cast<void>(SDL_RaiseWindow(window_));
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - GameShark Cheats", 760, 660,
                                   SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) sdl_error("Could not create cheat manager window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 680, 560));
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            close();
            sdl_error("Could not create cheat manager renderer");
        }
        if (parent != nullptr) {
            int x = 0;
            int y = 0;
            static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
            static_cast<void>(SDL_SetWindowPosition(window_, x + 56, y + 56));
        }
        if (cheats_.empty() && !archive_attempted_) {
            archive_attempted_ = true;
            fetch_requested_ = true;
            status_ = "Looking up this ROM in the Libretro cheat archive...";
        }
    }

    void close() noexcept {
        stop_editing();
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        renderer_ = nullptr;
        window_ = nullptr;
    }

    bool handle_event(const SDL_Event& event) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        SDL_WindowID event_window = 0;
        if (event.type >= SDL_EVENT_WINDOW_FIRST &&
            event.type <= SDL_EVENT_WINDOW_LAST) {
            event_window = event.window.windowID;
        } else if (event.type == SDL_EVENT_KEY_DOWN ||
                   event.type == SDL_EVENT_KEY_UP) {
            event_window = event.key.windowID;
        } else if (event.type == SDL_EVENT_TEXT_INPUT) {
            event_window = event.text.windowID;
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                   event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            event_window = event.button.windowID;
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            event_window = event.wheel.windowID;
        }
        if (event_window != id) return false;

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close();
        } else if (event.type == SDL_EVENT_TEXT_INPUT && editing_ != Field::none) {
            auto& target = editing_ == Field::description ? description_ : code_;
            target += event.text.text;
            if (target.size() > 160) target.resize(160);
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (editing_ != Field::none) {
                auto& target = editing_ == Field::description ? description_ : code_;
                if (event.key.key == SDLK_ESCAPE) stop_editing();
                else if (event.key.key == SDLK_BACKSPACE && !target.empty()) {
                    target.pop_back();
                } else if (event.key.key == SDLK_TAB) {
                    begin_edit(editing_ == Field::description ? Field::code
                                                              : Field::description);
                } else if (event.key.key == SDLK_RETURN ||
                           event.key.key == SDLK_KP_ENTER) {
                    add_manual();
                }
            } else if (event.key.key == SDLK_ESCAPE) {
                close();
            } else if (event.key.key == SDLK_UP && !cheats_.empty()) {
                if (selected_ > 0) --selected_;
                reveal_selected();
            } else if (event.key.key == SDLK_DOWN && !cheats_.empty()) {
                if (selected_ + 1 < cheats_.size()) ++selected_;
                reveal_selected();
            } else if (event.key.key == SDLK_SPACE && !cheats_.empty()) {
                cheats_[selected_].enabled = !cheats_[selected_].enabled;
                save();
            } else if (event.key.key == SDLK_DELETE && !cheats_.empty()) {
                erase_selected();
            }
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            if (event.wheel.y > 0 && scroll_ > 0) --scroll_;
            if (event.wheel.y < 0 && scroll_ + visible_rows_ < cheats_.size()) {
                ++scroll_;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            const auto x = event.button.x;
            const auto y = event.button.y;
            const auto list_bottom = static_cast<float>(height - 182);
            if (y >= 92 && y < list_bottom) {
                const auto row = static_cast<std::size_t>((y - 92) / 30);
                const auto index = scroll_ + row;
                if (index < cheats_.size()) {
                    selected_ = index;
                    if (x < 60) {
                        cheats_[index].enabled = !cheats_[index].enabled;
                        save();
                    }
                }
            } else if (y >= height - 150 && y <= height - 120) {
                if (x >= 24 && x < width / 2 - 8) begin_edit(Field::description);
                else if (x >= width / 2 + 8 && x <= width - 24) {
                    begin_edit(Field::code);
                }
            } else if (y >= height - 100 && y <= height - 64) {
                if (x >= 24 && x <= 178) fetch_requested_ = true;
                else if (x >= 194 && x <= 334) add_manual();
                else if (x >= 350 && x <= 490) erase_selected();
                else if (x >= width - 144 && x <= width - 24) close();
            }
        }
        return true;
    }

    void fetch_archive() {
        if (directory_.empty()) throw std::runtime_error("No ROM is active");
        archive_attempted_ = true;
        std::filesystem::create_directories(directory_);
        const auto remote = directory_ / "archive-download.cht";
        const auto system = std::string{gameboy::cover_system_name(metadata_.platform)};
        const auto canonical_name = resolve_canonical_name(system);
        const auto url = "https://raw.githubusercontent.com/libretro/"
                         "libretro-database/master/cht/" +
                         url_component(system) + "/" +
                         url_component(canonical_name + ".cht");
        std::string error;
        if (!gbb_desktop::download_public_file(url, remote, 4 * 1024 * 1024,
                                                error)) {
            status_ = "No exact archive match. Manual codes are still available.";
            throw std::runtime_error(
                "No exact Libretro cheat archive match was found for:\n" +
                canonical_name +
                "\n\nUse a No-Intro named ROM or add a code manually.\n" + error);
        }
        std::string text;
        {
            std::ifstream input(remote, std::ios::binary);
            if (!input) {
                throw std::runtime_error(
                    "Could not open the downloaded cheat archive");
            }
            text.assign(std::istreambuf_iterator<char>{input}, {});
            if (input.bad()) {
                throw std::runtime_error(
                    "Could not read the downloaded cheat archive");
            }
        } // Close the stream before deleting the file (required on Windows).
        std::error_code cleanup_error;
        std::filesystem::remove(remote, cleanup_error);
        auto imported = gameboy::parse_libretro_cheats(text, true);
        if (imported.empty()) {
            status_ = "The archive has no supported type-01 codes.";
            throw std::runtime_error(
                "The archive file contains no supported Game Boy type-01 codes");
        }
        std::size_t added = 0;
        for (auto& cheat : imported) {
            const auto duplicate = std::any_of(
                cheats_.begin(), cheats_.end(), [&](const auto& existing) {
                    return existing.code == cheat.code;
                });
            if (!duplicate) {
                cheats_.push_back(std::move(cheat));
                ++added;
            }
        }
        save();
        status_ = std::to_string(added) + " archive cheats added (" +
                  std::to_string(cheats_.size()) + " total).";
    }

    void present() {
        if (!visible()) return;
        int width = 0;
        int height = 0;
        static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
        visible_rows_ = std::max<std::size_t>(1, (height - 274) / 30);
        if (scroll_ + visible_rows_ > cheats_.size() &&
            cheats_.size() > visible_rows_) {
            scroll_ = cheats_.size() - visible_rows_;
        }
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        text(24, 20, "GAMESHARK CHEATS / " + metadata_.title, 69, 207, 238);
        text(24, 42, "ROM-aware source: Libretro Database (CC BY-SA 4.0)",
             177, 192, 208);
        text(24, 64, status_, 177, 192, 208);
        text(26, 82, "ON", 238, 249, 255);
        text(62, 82, "DESCRIPTION", 238, 249, 255);
        text(static_cast<float>(width - 210), 82, "SOURCE", 238, 249, 255);
        for (std::size_t row = 0; row < visible_rows_; ++row) {
            const auto index = scroll_ + row;
            if (index >= cheats_.size()) break;
            const auto y = 92.0F + static_cast<float>(row) * 30.0F;
            const SDL_FRect background{20, y, static_cast<float>(width - 40), 27};
            const auto selected = index == selected_;
            static_cast<void>(SDL_SetRenderDrawColor(
                renderer_, selected ? 28 : 14, selected ? 47 : 26,
                selected ? 68 : 38, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &background));
            text(29, y + 9, cheats_[index].enabled ? "[X]" : "[ ]",
                 cheats_[index].enabled ? 69 : 177,
                 cheats_[index].enabled ? 207 : 192,
                 cheats_[index].enabled ? 238 : 208);
            text(62, y + 9, shortened(cheats_[index].description, 64),
                 238, 249, 255);
            text(static_cast<float>(width - 210), y + 9,
                 cheats_[index].from_archive ? "LIBRETRO" : "MANUAL",
                 177, 192, 208);
        }
        const auto field_y = static_cast<float>(height - 150);
        field({24, field_y, static_cast<float>(width / 2 - 32), 30},
              description_.empty() ? "Description" : description_,
              editing_ == Field::description);
        field({static_cast<float>(width / 2 + 8), field_y,
               static_cast<float>(width / 2 - 32), 30},
              code_.empty() ? "Code: 01VVLLHH[+...]" : code_,
              editing_ == Field::code);
        const auto button_y = static_cast<float>(height - 100);
        button({24, button_y, 154, 36}, "FETCH FOR ROM");
        button({194, button_y, 140, 36}, "ADD MANUAL");
        button({350, button_y, 140, 36}, "DELETE");
        button({static_cast<float>(width - 144), button_y, 120, 36}, "CLOSE");
        text(24, static_cast<float>(height - 42),
             "Click [ ] to toggle. Up/Down select, Space toggles, Delete removes.",
             177, 192, 208);
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

private:
    enum class Field { none, description, code };

    static std::string url_component(const std::string& value) {
        std::ostringstream output;
        output << std::uppercase << std::hex;
        for (const auto raw : value) {
            const auto character = static_cast<unsigned char>(raw);
            if (std::isalnum(character) || character == '-' || character == '_' ||
                character == '.' || character == '~') {
                output << static_cast<char>(character);
            } else {
                output << '%' << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(character);
            }
        }
        return output.str();
    }

    static std::string shortened(const std::string& value,
                                 const std::size_t maximum) {
        if (value.size() <= maximum) return value;
        return value.substr(0, maximum - 3) + "...";
    }

    static std::string quoted_value(const std::string& line) {
        const auto first = line.find('"');
        const auto last = line.rfind('"');
        return first == std::string::npos || last <= first
                   ? std::string{}
                   : line.substr(first + 1, last - first - 1);
    }

    std::string resolve_canonical_name(const std::string& system) const {
        auto filename = system;
        for (auto& character : filename) {
            if (!std::isalnum(static_cast<unsigned char>(character))) character = '-';
        }
        const auto database = preference_directory_ / "metadata" /
                              (filename + ".dat");
        if (!std::filesystem::is_regular_file(database)) {
            std::string ignored_error;
            const auto url =
                "https://raw.githubusercontent.com/libretro/libretro-database/"
                "master/metadat/no-intro/" + url_component(system + ".dat");
            static_cast<void>(gbb_desktop::download_public_file(
                url, database, 3 * 1024 * 1024, ignored_error));
        }
        std::ifstream input(database);
        std::string line;
        std::string name;
        while (std::getline(input, line)) {
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;
            line.erase(0, first);
            if (line == "game (") {
                name.clear();
            } else if (line.rfind("name \"", 0) == 0 && name.empty()) {
                name = quoted_value(line);
            } else if (line.rfind("rom (", 0) == 0 && !name.empty()) {
                const auto marker = line.find(" crc ");
                if (marker == std::string::npos || marker + 13 > line.size()) {
                    continue;
                }
                std::uint32_t crc{};
                std::istringstream value(line.substr(marker + 5, 8));
                if ((value >> std::hex >> crc) && crc == metadata_.crc32) {
                    return name;
                }
            }
        }
        return metadata_.cover_name;
    }

    void text(const float x, const float y, const std::string& value,
              const std::uint8_t r, const std::uint8_t g,
              const std::uint8_t b) const {
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, r, g, b, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, x, y, value.c_str()));
    }

    void button(const SDL_FRect& rect, const char* label) const {
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 28, 47, 68, 255));
        static_cast<void>(SDL_RenderFillRect(renderer_, &rect));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &rect));
        text(rect.x + 10, rect.y + 14, label, 238, 249, 255);
    }

    void field(const SDL_FRect& rect, const std::string& value,
               const bool active) const {
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 14, 26, 38, 255));
        static_cast<void>(SDL_RenderFillRect(renderer_, &rect));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, active ? 69 : 69,
                                                 active ? 207 : 112,
                                                 active ? 238 : 133, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &rect));
        text(rect.x + 8, rect.y + 11, shortened(value, 42), 238, 249, 255);
    }

    void begin_edit(const Field field) {
        editing_ = field;
        static_cast<void>(SDL_StartTextInput(window_));
    }

    void stop_editing() noexcept {
        if (window_ != nullptr && editing_ != Field::none) SDL_StopTextInput(window_);
        editing_ = Field::none;
    }

    void add_manual() {
        try {
            auto writes = gameboy::parse_gameshark_code(code_);
            auto description = description_.empty() ? code_ : description_;
            cheats_.push_back({std::move(description), code_, true, false,
                               std::move(writes)});
            selected_ = cheats_.size() - 1;
            reveal_selected();
            description_.clear();
            code_.clear();
            stop_editing();
            save();
            status_ = "Manual cheat added and enabled.";
        } catch (const std::exception& error) {
            status_ = std::string{"Invalid code: "} + error.what();
        }
    }

    void erase_selected() {
        if (cheats_.empty()) return;
        cheats_.erase(cheats_.begin() + static_cast<std::ptrdiff_t>(selected_));
        if (selected_ >= cheats_.size() && selected_ > 0) --selected_;
        reveal_selected();
        save();
        status_ = "Cheat removed.";
    }

    void reveal_selected() {
        if (selected_ < scroll_) scroll_ = selected_;
        if (selected_ >= scroll_ + visible_rows_) {
            scroll_ = selected_ - visible_rows_ + 1;
        }
    }

    void save() const {
        if (path_.empty()) return;
        std::filesystem::create_directories(path_.parent_path());
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not save the ROM cheat list");
        output << gameboy::serialize_libretro_cheats(cheats_);
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    gameboy::RomMetadata metadata_{};
    std::filesystem::path preference_directory_;
    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::vector<gameboy::GameSharkCheat> cheats_;
    std::size_t selected_{};
    std::size_t scroll_{};
    std::size_t visible_rows_{10};
    Field editing_{Field::none};
    std::string description_;
    std::string code_;
    std::string status_;
    bool fetch_requested_{};
    bool archive_attempted_{};
};

class DesktopDebugger {
public:
    DesktopDebugger() = default;
    ~DesktopDebugger() { close(); }
    DesktopDebugger(const DesktopDebugger&) = delete;
    DesktopDebugger& operator=(const DesktopDebugger&) = delete;

    [[nodiscard]] bool visible() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool execution_paused() const noexcept {
        return visible() && execution_paused_;
    }
    [[nodiscard]] bool take_instruction_step() noexcept {
        return std::exchange(step_instruction_, false);
    }
    [[nodiscard]] bool take_frame_step() noexcept {
        return std::exchange(step_frame_, false);
    }
    [[nodiscard]] bool take_record_toggle() noexcept {
        return std::exchange(toggle_recording_, false);
    }
    [[nodiscard]] bool take_replay_request() noexcept {
        return std::exchange(replay_requested_, false);
    }
    [[nodiscard]] bool take_tas_request() noexcept {
        return std::exchange(tas_requested_, false);
    }
    [[nodiscard]] bool take_sprite_request() noexcept {
        return std::exchange(sprite_requested_, false);
    }
    void request_record_toggle() noexcept { toggle_recording_ = true; }
    void request_replay() noexcept { replay_requested_ = true; }
    void request_tas_editor() noexcept { tas_requested_ = true; }
    void request_sprite_editor() noexcept { sprite_requested_ = true; }
    void run() noexcept { execution_paused_ = false; }
    void pause() noexcept { execution_paused_ = true; }

    void toggle(SDL_Window* parent) {
        if (visible()) {
            close();
            return;
        }
        window_ = SDL_CreateWindow("Go Bigger Boy - Debugger", 920, 760,
                                   SDL_WINDOW_RESIZABLE);
        if (window_ == nullptr) sdl_error("Could not create debugger window");
        static_cast<void>(SDL_SetWindowMinimumSize(window_, 920, 720));
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (renderer_ == nullptr) {
            close();
            sdl_error("Could not create debugger renderer");
        }
        texture_ = SDL_CreateTexture(
            renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
            static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height));
        if (texture_ == nullptr) {
            close();
            sdl_error("Could not create debugger framebuffer texture");
        }
        static_cast<void>(SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST));
        execution_paused_ = true;
        if (parent != nullptr) {
            int x = 0;
            int y = 0;
            static_cast<void>(SDL_GetWindowPosition(parent, &x, &y));
            static_cast<void>(SDL_SetWindowPosition(window_, x + 48, y + 48));
        }
    }

    void close() noexcept {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        texture_ = nullptr;
        renderer_ = nullptr;
        window_ = nullptr;
        execution_paused_ = true;
        step_instruction_ = false;
        step_frame_ = false;
        toggle_recording_ = false;
        replay_requested_ = false;
        tas_requested_ = false;
        sprite_requested_ = false;
        cancel_edit();
    }

    bool handle_event(const SDL_Event& event, gameboy::Emulator* emulator) {
        if (!visible()) return false;
        const auto id = SDL_GetWindowID(window_);
        SDL_WindowID event_window = 0;
        if (event.type >= SDL_EVENT_WINDOW_FIRST &&
            event.type <= SDL_EVENT_WINDOW_LAST) {
            event_window = event.window.windowID;
        } else if (event.type == SDL_EVENT_KEY_DOWN ||
                   event.type == SDL_EVENT_KEY_UP) {
            event_window = event.key.windowID;
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                   event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            event_window = event.button.windowID;
        }
        if (event_window != id) return false;

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            close();
        } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
            if (editing_) {
                if (event.key.key == SDLK_ESCAPE) {
                    cancel_edit();
                } else if (event.key.key == SDLK_RETURN ||
                           event.key.key == SDLK_KP_ENTER) {
                    apply_edit(emulator);
                } else if (event.key.key == SDLK_BACKSPACE) {
                    if (replace_on_type_) {
                        edit_value_.clear();
                        replace_on_type_ = false;
                    } else if (!edit_value_.empty()) {
                        edit_value_.pop_back();
                    }
                } else if (const auto digit = hexadecimal_digit(event.key.key)) {
                    if (replace_on_type_) {
                        edit_value_.clear();
                        replace_on_type_ = false;
                    }
                    const auto maximum = register_width(*editing_);
                    if (edit_value_.size() < maximum) edit_value_ += *digit;
                }
            } else if (event.key.key == SDLK_F12 || event.key.key == SDLK_ESCAPE) {
                close();
            } else if (event.key.key == SDLK_F5 || event.key.key == SDLK_SPACE) {
                execution_paused_ = !execution_paused_;
            } else if (event.key.key == SDLK_F10) {
                execution_paused_ = true;
                step_instruction_ = true;
            } else if (event.key.key == SDLK_F11) {
                execution_paused_ = true;
                step_frame_ = true;
            } else if (event.key.key == SDLK_F6) {
                toggle_recording_ = true;
            } else if (event.key.key == SDLK_F7) {
                replay_requested_ = true;
            } else if (event.key.key == SDLK_F8) {
                tas_requested_ = true;
            } else if (event.key.key == SDLK_F9) {
                sprite_requested_ = true;
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                   event.button.button == SDL_BUTTON_LEFT) {
            int width = 0;
            int height = 0;
            static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
            const auto y = static_cast<float>(height - 58);
            const auto movie_y = static_cast<float>(height - 106);
            const auto x = event.button.x;
            const auto register_x =
                std::max(530.0F, static_cast<float>(width) - 350.0F);
            if (emulator != nullptr && execution_paused_) {
                if (const auto selected = register_at(
                        event.button.x, event.button.y, register_x)) {
                    begin_edit(*selected, emulator->cpu().registers());
                    return true;
                }
            }
            cancel_edit();
            if (event.button.y >= movie_y &&
                event.button.y <= movie_y + 36.0F) {
                if (x >= 24.0F && x <= 194.0F) {
                    toggle_recording_ = true;
                } else if (x >= 208.0F && x <= 378.0F) {
                    replay_requested_ = true;
                } else if (x >= 734.0F && x <= 884.0F) {
                    tas_requested_ = true;
                }
            } else if (event.button.y >= y && event.button.y <= y + 36.0F) {
                if (x >= 24.0F && x <= 174.0F) {
                    execution_paused_ = !execution_paused_;
                } else if (x >= 188.0F && x <= 358.0F) {
                    execution_paused_ = true;
                    step_instruction_ = true;
                } else if (x >= 372.0F && x <= 522.0F) {
                    execution_paused_ = true;
                    step_frame_ = true;
                } else if (x >= 536.0F && x <= 686.0F) {
                    sprite_requested_ = true;
                }
            }
        }
        return true;
    }

    void present(const gameboy::Emulator& emulator,
                 const gameboy::DisplayPalette& palette,
                 const InputMovie& movie) {
        if (!visible()) return;
        int width = 0;
        int height = 0;
        static_cast<void>(SDL_GetWindowSize(window_, &width, &height));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 8, 12, 20, 255));
        static_cast<void>(SDL_RenderClear(renderer_));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderDebugText(renderer_, 24, 20,
                                              "GO BIGGER BOY / DEBUGGER"));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 177, 192, 208, 255));
        static_cast<void>(SDL_RenderDebugText(
            renderer_, 24, 38, execution_paused_ ? "PAUSED" : "RUNNING"));

        constexpr float scale = 3.0F;
        constexpr float preview_x = 24.0F;
        constexpr float preview_y = 72.0F;
        constexpr float preview_width = gameboy::Ppu::screen_width * scale;
        constexpr float preview_height = gameboy::Ppu::screen_height * scale;
        gameboy::Ppu::Framebuffer pixels{};
        const auto& source = emulator.framebuffer();
        const auto native = emulator.bus().cgb_mode() || palette.cgb_compatibility;
        for (std::size_t index = 0; index < source.size(); ++index) {
            pixels[index] = native ? source[index]
                                   : gameboy::apply_display_palette(source[index],
                                                                    palette);
        }
        static_cast<void>(SDL_UpdateTexture(
            texture_, nullptr, pixels.data(),
            static_cast<int>(gameboy::Ppu::screen_width * sizeof(std::uint32_t))));
        const SDL_FRect preview{preview_x, preview_y, preview_width,
                                preview_height};
        static_cast<void>(SDL_RenderTexture(renderer_, texture_, nullptr, &preview));
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
        static_cast<void>(SDL_RenderRect(renderer_, &preview));
        const SDL_FRect outer{preview_x - 3, preview_y - 3,
                              preview_width + 6, preview_height + 6};
        static_cast<void>(SDL_RenderRect(renderer_, &outer));
        static_cast<void>(SDL_RenderDebugText(renderer_, preview_x,
                                              preview_y + preview_height + 10,
                                              "VISIBLE VIEWPORT 160 x 144"));

        const auto& r = emulator.cpu().registers();
        const auto pair = [](const std::uint8_t high, const std::uint8_t low) {
            return static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(high) << 8U) | low);
        };
        const auto hex8 = [](const std::uint8_t value) {
            std::ostringstream out;
            out << '$' << std::uppercase << std::hex << std::setfill('0')
                << std::setw(2) << static_cast<unsigned>(value);
            return out.str();
        };
        const auto hex16 = [](const std::uint16_t value) {
            std::ostringstream out;
            out << '$' << std::uppercase << std::hex << std::setfill('0')
                << std::setw(4) << value;
            return out.str();
        };
        const auto text = [this](const float x, const float y,
                                 const std::string& value) {
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 230, 249, 255, 255));
            static_cast<void>(SDL_RenderDebugText(renderer_, x, y, value.c_str()));
        };
        const auto register_x = std::max(530.0F, static_cast<float>(width) - 350.0F);
        text(register_x, 72, "CPU REGISTERS");
        text(register_x, 86, execution_paused_
                                 ? "CLICK A VALUE TO EDIT"
                                 : "PAUSE TO EDIT REGISTERS");
        text(register_x, 100, "A  " + hex8(r.a) + "    F  " + hex8(r.f));
        text(register_x, 122, "B  " + hex8(r.b) + "    C  " + hex8(r.c));
        text(register_x, 144, "D  " + hex8(r.d) + "    E  " + hex8(r.e));
        text(register_x, 166, "H  " + hex8(r.h) + "    L  " + hex8(r.l));
        text(register_x, 198, "AF " + hex16(pair(r.a, r.f)));
        text(register_x, 220, "BC " + hex16(pair(r.b, r.c)));
        text(register_x, 242, "DE " + hex16(pair(r.d, r.e)));
        text(register_x, 264, "HL " + hex16(pair(r.h, r.l)));
        text(register_x, 296, "SP " + hex16(r.sp));
        text(register_x, 318, "PC " + hex16(r.pc));
        constexpr std::array editable_registers{
            RegisterTarget::a, RegisterTarget::f, RegisterTarget::b,
            RegisterTarget::c, RegisterTarget::d, RegisterTarget::e,
            RegisterTarget::h, RegisterTarget::l, RegisterTarget::sp,
            RegisterTarget::pc};
        static_cast<void>(SDL_SetRenderDrawColor(renderer_, 34, 91, 111, 255));
        for (const auto target : editable_registers) {
            const auto [x, y] = register_position(target, register_x);
            const SDL_FRect box{x - 3.0F, y - 3.0F,
                                register_width(target) == 2 ? 38.0F : 54.0F,
                                14.0F};
            static_cast<void>(SDL_RenderRect(renderer_, &box));
        }
        if (editing_) {
            const auto [x, y] = register_position(*editing_, register_x);
            const SDL_FRect edit_box{x - 3.0F, y - 3.0F,
                                     register_width(*editing_) == 2 ? 38.0F
                                                                    : 54.0F,
                                     14.0F};
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 20, 77, 101, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &edit_box));
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
            static_cast<void>(SDL_RenderRect(renderer_, &edit_box));
            text(x, y, "$" + edit_value_ + "_");
        }
        text(register_x, 350,
             std::string("FLAGS  Z:") + ((r.f & 0x80U) ? "1" : "0") +
                 " N:" + ((r.f & 0x40U) ? "1" : "0") +
                 " H:" + ((r.f & 0x20U) ? "1" : "0") +
                 " C:" + ((r.f & 0x10U) ? "1" : "0"));
        text(register_x, 372, std::string("IME ") +
                                  (emulator.cpu().interrupts_enabled() ? "ON" : "OFF"));
        text(register_x, 394,
             std::string("CPU ") +
                 (emulator.cpu().stopped()
                      ? "STOPPED"
                      : emulator.cpu().halted() ? "HALTED" : "ACTIVE"));
        text(register_x, 426,
             "CYCLES " + std::to_string(emulator.cpu().total_cycles()));
        const auto& bus = emulator.bus();
        text(register_x, 464, "HARDWARE REGISTERS");
        text(register_x, 486,
             "LCDC " + hex8(bus.read8(0xFF40)) +
                 "  STAT " + hex8(bus.read8(0xFF41)));
        text(register_x, 508,
             "SCX  " + hex8(bus.read8(0xFF43)) +
                 "  SCY  " + hex8(bus.read8(0xFF42)));
        text(register_x, 530,
             "LY   " + hex8(bus.read8(0xFF44)) +
                 "  LYC  " + hex8(bus.read8(0xFF45)));
        text(register_x, 552,
             "WX   " + hex8(bus.read8(0xFF4B)) +
                 "  WY   " + hex8(bus.read8(0xFF4A)));
        text(register_x, 574,
             "IF   " + hex8(bus.read8(0xFF0F)) +
                 "  IE   " + hex8(bus.read8(0xFFFF)));

        const auto button = [this](const SDL_FRect& rect,
                                   const std::string& label) {
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 28, 47, 68, 255));
            static_cast<void>(SDL_RenderFillRect(renderer_, &rect));
            static_cast<void>(SDL_SetRenderDrawColor(renderer_, 69, 207, 238, 255));
            static_cast<void>(SDL_RenderRect(renderer_, &rect));
            static_cast<void>(SDL_RenderDebugText(renderer_, rect.x + 12,
                                                  rect.y + 14, label.c_str()));
        };
        const auto button_y = static_cast<float>(height - 58);
        const auto movie_y = static_cast<float>(height - 106);
        button({24, movie_y, 170, 36},
               movie.recording() ? "F6 STOP + SAVE" : "F6 START RECORDING");
        button({208, movie_y, 170, 36}, "F7 REPLAY LAST");
        text(398, movie_y + 14,
             movie.replaying()
                 ? "REPLAYING"
                 : movie.recording()
                       ? "RECORDING  EVENTS " +
                             std::to_string(movie.event_count())
                       : "INPUT MOVIE IDLE");
        button({734, movie_y, 150, 36}, "F8 TAS EDITOR");
        button({24, button_y, 150, 36}, execution_paused_ ? "F5  RUN" : "F5  PAUSE");
        button({188, button_y, 170, 36}, "F10 STEP INSTRUCTION");
        button({372, button_y, 150, 36}, "F11 STEP FRAME");
        button({536, button_y, 150, 36}, "F9 SPRITE EDITOR");
        text(static_cast<float>(width) - 170.0F, button_y + 14, "F12 CLOSE");
        static_cast<void>(SDL_RenderPresent(renderer_));
    }

private:
    enum class RegisterTarget { a, f, b, c, d, e, h, l, sp, pc };

    [[nodiscard]] static std::optional<char> hexadecimal_digit(
        const SDL_Keycode key) noexcept {
        if (key >= SDLK_0 && key <= SDLK_9) {
            return static_cast<char>('0' + (key - SDLK_0));
        }
        if (key >= SDLK_A && key <= SDLK_F) {
            return static_cast<char>('A' + (key - SDLK_A));
        }
        if (key >= SDLK_KP_0 && key <= SDLK_KP_9) {
            return static_cast<char>('0' + (key - SDLK_KP_0));
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::size_t register_width(
        const RegisterTarget target) noexcept {
        return target == RegisterTarget::sp || target == RegisterTarget::pc
                   ? 4U
                   : 2U;
    }

    [[nodiscard]] static std::pair<float, float> register_position(
        const RegisterTarget target, const float x) noexcept {
        switch (target) {
        case RegisterTarget::a: return {x + 24, 100};
        case RegisterTarget::f: return {x + 104, 100};
        case RegisterTarget::b: return {x + 24, 122};
        case RegisterTarget::c: return {x + 104, 122};
        case RegisterTarget::d: return {x + 24, 144};
        case RegisterTarget::e: return {x + 104, 144};
        case RegisterTarget::h: return {x + 24, 166};
        case RegisterTarget::l: return {x + 104, 166};
        case RegisterTarget::sp: return {x + 24, 296};
        case RegisterTarget::pc: return {x + 24, 318};
        }
        return {x, 0};
    }

    [[nodiscard]] static std::optional<RegisterTarget> register_at(
        const float mouse_x, const float mouse_y, const float register_x) noexcept {
        constexpr std::array targets{
            RegisterTarget::a, RegisterTarget::f, RegisterTarget::b,
            RegisterTarget::c, RegisterTarget::d, RegisterTarget::e,
            RegisterTarget::h, RegisterTarget::l, RegisterTarget::sp,
            RegisterTarget::pc};
        for (const auto target : targets) {
            const auto [x, y] = register_position(target, register_x);
            const auto width = register_width(target) == 2 ? 38.0F : 54.0F;
            if (mouse_x >= x - 4.0F && mouse_x <= x + width &&
                mouse_y >= y - 5.0F && mouse_y <= y + 13.0F) {
                return target;
            }
        }
        return std::nullopt;
    }

    void begin_edit(const RegisterTarget target,
                    const gameboy::CpuRegisters& registers) {
        std::uint16_t value = 0;
        switch (target) {
        case RegisterTarget::a: value = registers.a; break;
        case RegisterTarget::f: value = registers.f; break;
        case RegisterTarget::b: value = registers.b; break;
        case RegisterTarget::c: value = registers.c; break;
        case RegisterTarget::d: value = registers.d; break;
        case RegisterTarget::e: value = registers.e; break;
        case RegisterTarget::h: value = registers.h; break;
        case RegisterTarget::l: value = registers.l; break;
        case RegisterTarget::sp: value = registers.sp; break;
        case RegisterTarget::pc: value = registers.pc; break;
        }
        std::ostringstream out;
        out << std::uppercase << std::hex << std::setfill('0')
            << std::setw(static_cast<int>(register_width(target))) << value;
        editing_ = target;
        edit_value_ = out.str();
        replace_on_type_ = true;
    }

    void apply_edit(gameboy::Emulator* emulator) {
        if (!editing_ || emulator == nullptr || edit_value_.empty()) return;
        const auto value = static_cast<std::uint16_t>(
            std::stoul(edit_value_, nullptr, 16));
        auto registers = emulator->cpu().registers();
        switch (*editing_) {
        case RegisterTarget::a: registers.a = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::f: registers.f = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::b: registers.b = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::c: registers.c = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::d: registers.d = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::e: registers.e = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::h: registers.h = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::l: registers.l = static_cast<std::uint8_t>(value); break;
        case RegisterTarget::sp: registers.sp = value; break;
        case RegisterTarget::pc: registers.pc = value; break;
        }
        emulator->set_cpu_registers(registers);
        cancel_edit();
    }

    void cancel_edit() noexcept {
        editing_.reset();
        edit_value_.clear();
        replace_on_type_ = true;
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    bool execution_paused_{true};
    bool step_instruction_{};
    bool step_frame_{};
    bool toggle_recording_{};
    bool replay_requested_{};
    bool tas_requested_{};
    bool sprite_requested_{};
    std::optional<RegisterTarget> editing_;
    std::string edit_value_;
    bool replace_on_type_{true};
};
#endif

bool configure_video_pipeline(SdlResources& sdl,
                              const gameboy::VideoMode mode) {
    const auto presentation = mode == gameboy::VideoMode::integer
                                  ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                  : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    const auto filtering = mode == gameboy::VideoMode::bilinear
                               ? SDL_SCALEMODE_LINEAR
                               : SDL_SCALEMODE_NEAREST;
    if (!SDL_SetRenderLogicalPresentation(
            sdl.renderer, static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height), presentation) ||
        !SDL_SetTextureScaleMode(sdl.texture, filtering)) {
        return false;
    }
    sdl.video_mode = mode;
    return true;
}

struct DialogState {
    std::mutex mutex;
    bool active{};
    std::optional<std::string> selected_path;
    std::optional<std::string> error;
};

constexpr std::array<gameboy::Button, 8> button_order{
    gameboy::Button::right, gameboy::Button::left, gameboy::Button::up,
    gameboy::Button::down, gameboy::Button::a, gameboy::Button::b,
    gameboy::Button::select, gameboy::Button::start,
};

constexpr std::array<const char*, 8> button_names{
    "Right", "Left", "Up", "Down", "A", "B", "Select", "Start",
};

constexpr std::array<const char*, 4> shortcut_names{
    "FastForward", "Rewind", "SaveState", "LoadState",
};

constexpr std::size_t shortcut_fast_forward = 0;
constexpr std::size_t shortcut_rewind = 1;
constexpr std::size_t shortcut_save_state = 2;
constexpr std::size_t shortcut_load_state = 3;

struct InputBindings {
    std::array<std::array<SDL_Keycode, 2>, 8> keys{{
        {{SDLK_RIGHT, SDLK_UNKNOWN}}, {{SDLK_LEFT, SDLK_UNKNOWN}},
        {{SDLK_UP, SDLK_UNKNOWN}}, {{SDLK_DOWN, SDLK_UNKNOWN}},
        {{SDLK_X, SDLK_UNKNOWN}}, {{SDLK_Z, SDLK_UNKNOWN}},
        {{SDLK_BACKSPACE, SDLK_UNKNOWN}}, {{SDLK_RETURN, SDLK_UNKNOWN}},
    }};
    std::array<SDL_GamepadButton, 8> gamepad_buttons{
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START,
    };
    std::array<SDL_Keycode, shortcut_names.size()> shortcuts{
        SDLK_TAB, SDLK_LSHIFT, SDLK_F5, SDLK_F8};
};

enum class BindingDevice { keyboard, gamepad };

struct BindingConfiguration {
    BindingDevice device{};
    std::size_t index{};
    std::size_t slot{};
};

enum class DashboardAction {
    resume, open_rom, palette, video, shortcuts, recent_rom, quit
};

struct DashboardItem {
    DashboardAction action{};
    std::size_t recent_index{};
    std::string label;
};

constexpr std::size_t dashboard_visible_rows = 5;
constexpr float dashboard_first_row_y = 39.0F;
constexpr float dashboard_row_height = 18.0F;

constexpr std::uintmax_t maximum_quick_state_size = 2 * 1024 * 1024;
constexpr std::size_t maximum_rewind_frames = 180;
constexpr unsigned fast_forward_factor = 4;
using RewindHistory = std::deque<std::vector<std::uint8_t>>;

std::filesystem::path preference_directory() {
#ifdef _WIN32
    // Keep the Windows build portable: settings, saves, recent-ROM metadata,
    // printer output, quick states, and updater files all live beside gbb.exe.
    // SDL_GetPrefPath() resolves to AppData on Windows, which makes a portable
    // installation unexpectedly split across two locations.
    const auto* raw_path = SDL_GetBasePath();
    if (raw_path == nullptr) return {};
    const auto path = std::filesystem::u8path(raw_path);
    return path.lexically_normal();
#else
    char* raw_path = SDL_GetPrefPath("Go Bigger Boy", "GBB");
    if (raw_path == nullptr) return {};
    const auto path = std::filesystem::u8path(raw_path);
    SDL_free(raw_path);
    return path;
#endif
}

struct WindowGeometry {
    int x{};
    int y{};
    int width{};
    int height{};
};

bool geometry_is_visible(const WindowGeometry& geometry) {
    int display_count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    if (displays == nullptr) return true;
    const SDL_Rect window{geometry.x, geometry.y, geometry.width,
                          geometry.height};
    bool visible = false;
    for (int index = 0; index < display_count; ++index) {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayBounds(displays[index], &bounds)) continue;
        SDL_Rect intersection{};
        if (SDL_GetRectIntersection(&window, &bounds, &intersection) &&
            intersection.w >= 64 && intersection.h >= 64) {
            visible = true;
            break;
        }
    }
    SDL_free(displays);
    return visible;
}

void restore_game_window_geometry(
    SDL_Window* window, const std::filesystem::path& directory) {
    if (directory.empty()) return;
    std::ifstream input(directory / "game-window.txt");
    WindowGeometry geometry;
    if (!(input >> geometry.x >> geometry.y >> geometry.width >>
          geometry.height) ||
        geometry.width < 320 || geometry.height < 288 ||
        !geometry_is_visible(geometry)) {
        return;
    }
    static_cast<void>(
        SDL_SetWindowSize(window, geometry.width, geometry.height));
    static_cast<void>(SDL_SetWindowPosition(window, geometry.x, geometry.y));
}

void save_game_window_geometry(
    SDL_Window* window, const std::filesystem::path& directory) {
    if (directory.empty() ||
        (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0) {
        return;
    }
    WindowGeometry geometry;
    if (!SDL_GetWindowPosition(window, &geometry.x, &geometry.y) ||
        !SDL_GetWindowSize(window, &geometry.width, &geometry.height) ||
        geometry.width < 320 || geometry.height < 288) {
        return;
    }
    std::ofstream output(directory / "game-window.txt", std::ios::trunc);
    output << geometry.x << ' ' << geometry.y << ' ' << geometry.width << ' '
           << geometry.height << '\n';
}

std::vector<std::string> load_legacy_recent_roms(
    const std::filesystem::path& directory) {
    std::vector<std::string> paths;
    if (directory.empty()) return paths;
    std::ifstream input(directory / "recent-roms.txt");
    std::string path;
    while (paths.size() < 9 && std::getline(input, path)) {
        if (!path.empty()) paths.push_back(path);
    }
    return paths;
}

std::vector<std::string> recent_paths(const gameboy::RomLibrary& library) {
    std::vector<std::string> paths;
    paths.reserve(library.entries().size());
    for (const auto& entry : library.entries()) {
        paths.push_back(entry.path.u8string());
    }
    return paths;
}

gameboy::RomLibrary load_rom_library(
    const std::filesystem::path& directory) {
    auto library = gameboy::RomLibrary::load(directory);
    if (!library.entries().empty()) return library;

    // Migrate the path-only dashboard history from older GBB versions. Invalid
    // or missing files are harmless and simply disappear from the new library.
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    for (const auto& path : load_legacy_recent_roms(directory)) {
        try {
            library.remember(path, gameboy::inspect_rom_file(path), timestamp--);
        } catch (const std::exception&) {
        }
    }
    if (!library.entries().empty()) library.save(directory);
    return library;
}

std::string dashboard_text(std::string text, const std::size_t maximum = 16) {
    for (auto& character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || byte > 126) character = '?';
    }
    if (text.size() > maximum) {
        text.resize(maximum > 3 ? maximum - 3 : maximum);
        if (maximum > 3) text += "...";
    }
    return text;
}

std::string rom_filename(const std::string& path) {
    auto name = std::filesystem::u8path(path).filename().u8string();
#ifdef __ANDROID__
    if (name.size() > 17 && name[16] == '-' &&
        std::all_of(name.begin(), name.begin() + 16, [](const char character) {
            return std::isxdigit(static_cast<unsigned char>(character)) != 0;
        })) {
        name.erase(0, 17);
    }
#endif
    return name.empty() ? path : name;
}

std::string rom_display_name(const std::string& path) {
    return dashboard_text(rom_filename(path));
}

std::vector<DashboardItem> dashboard_items(
    const bool can_resume, const std::vector<std::string>& recent) {
    std::vector<DashboardItem> items;
    items.reserve(recent.size() + 4);
    if (can_resume) {
        items.push_back({DashboardAction::resume, 0, "Resume game"});
    }
    items.push_back({DashboardAction::open_rom, 0, "+ Open a ROM"});
    items.push_back({DashboardAction::palette, 0, "Display palette"});
    items.push_back({DashboardAction::video, 0, "Video pipeline"});
    items.push_back({DashboardAction::shortcuts, 0, "Keyboard shortcuts"});
    for (std::size_t index = 0; index < recent.size(); ++index) {
        items.push_back({DashboardAction::recent_rom, index,
                         rom_display_name(recent[index])});
    }
    items.push_back({DashboardAction::quit, 0, "Exit GBB"});
    return items;
}

std::size_t dashboard_first_visible(const std::size_t selection,
                                    const std::size_t item_count) {
    if (item_count <= dashboard_visible_rows ||
        selection < dashboard_visible_rows) {
        return 0;
    }
    return std::min(selection - dashboard_visible_rows + 1,
                    item_count - dashboard_visible_rows);
}

InputBindings load_legacy_bindings(const std::filesystem::path& directory) {
    InputBindings bindings;
    if (directory.empty()) return bindings;
    std::ifstream input(directory / "controls.txt");
    auto loaded = bindings.keys;
    for (auto& keys : loaded) {
        long long value = 0;
        if (!(input >> value)) return bindings;
        keys[0] = static_cast<SDL_Keycode>(value);
    }
    std::array<SDL_Keycode, 8> unique_keys{};
    std::transform(loaded.begin(), loaded.end(), unique_keys.begin(),
                   [](const auto& keys) { return keys[0]; });
    if (std::find(unique_keys.begin(), unique_keys.end(), SDLK_UNKNOWN) !=
        unique_keys.end()) return bindings;
    std::sort(unique_keys.begin(), unique_keys.end());
    if (std::adjacent_find(unique_keys.begin(), unique_keys.end()) !=
        unique_keys.end()) {
        return bindings;
    }
    bindings.keys = loaded;

    // Older controls files contain only the eight keyboard bindings.
    auto loaded_gamepad = bindings.gamepad_buttons;
    for (auto& button : loaded_gamepad) {
        int value = 0;
        if (!(input >> value)) return bindings;
        if (value < 0 || value >= SDL_GAMEPAD_BUTTON_COUNT) return bindings;
        button = static_cast<SDL_GamepadButton>(value);
    }
    auto unique_buttons = loaded_gamepad;
    std::sort(unique_buttons.begin(), unique_buttons.end());
    if (std::adjacent_find(unique_buttons.begin(), unique_buttons.end()) !=
        unique_buttons.end()) {
        return bindings;
    }
    bindings.gamepad_buttons = loaded_gamepad;
    return bindings;
}

#ifdef __ANDROID__
void save_legacy_bindings(const std::filesystem::path& directory,
                          const InputBindings& bindings) {
    if (directory.empty()) return;
    std::ofstream output(directory / "controls.txt", std::ios::trunc);
    for (const auto& keys : bindings.keys) {
        output << static_cast<long long>(keys[0]) << '\n';
    }
    for (const auto button : bindings.gamepad_buttons) {
        output << static_cast<int>(button) << '\n';
    }
}
#endif

std::size_t load_legacy_display_palette(
    const std::filesystem::path& directory) {
    if (directory.empty()) return 0;
    std::ifstream input(directory / "palette.txt");
    std::string id;
    if (!(input >> id)) return 0;
    const auto found = std::find_if(
        gameboy::display_palettes.begin(), gameboy::display_palettes.end(),
        [&id](const gameboy::DisplayPalette& palette) {
            return id == palette.id;
        });
    return found == gameboy::display_palettes.end()
               ? 0
               : static_cast<std::size_t>(found -
                                          gameboy::display_palettes.begin());
}

#ifdef __ANDROID__
void save_legacy_display_palette(const std::filesystem::path& directory,
                                 const std::size_t palette) {
    if (directory.empty() || palette >= gameboy::display_palettes.size()) return;
    std::ofstream output(directory / "palette.txt", std::ios::trunc);
    output << gameboy::display_palettes[palette].id << '\n';
}
#endif

struct AppSettings {
    InputBindings bindings;
    std::size_t palette{};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    TouchControlSettings touch;
};

constexpr float minimum_touch_scale = 0.80F;
constexpr float maximum_touch_scale = 2.00F;
constexpr float minimum_touch_opacity = 0.20F;
constexpr float maximum_touch_opacity = 1.00F;
constexpr float minimum_touch_position = 0.02F;
constexpr float maximum_touch_position = 0.98F;
constexpr std::size_t touch_layout_count = 2;
constexpr std::size_t touch_control_count = 5;
constexpr std::size_t touch_layout_stride = touch_control_count * 2;
constexpr std::array<const char*, touch_layout_count> touch_layout_names{
    "Portrait", "Landscape"};
constexpr std::array<const char*, touch_control_count> touch_control_names{
    "DPad", "A", "B", "Select", "Start"};

float parse_touch_value(const std::string& value, const float fallback,
                        const float minimum, const float maximum) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stof(value, &consumed);
        if (consumed != value.size()) return fallback;
        return std::clamp(parsed, minimum, maximum);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::filesystem::path portable_settings_path(
    const std::filesystem::path& preference_directory) {
#ifdef __ANDROID__
    return preference_directory / "settings.ini";
#else
    const auto* raw_base = SDL_GetBasePath();
    if (raw_base == nullptr) return {};
    auto directory = std::filesystem::u8path(raw_base).lexically_normal();
    if (directory.filename().empty()) directory = directory.parent_path();
#ifdef __APPLE__
    directory = directory.parent_path().parent_path().parent_path();
#elif !defined(_WIN32)
    if (directory.filename() == "bin") directory = directory.parent_path();
#endif
    return directory / "settings.ini";
#endif
}

std::string trimmed_setting(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.resize(last + 1);
    return value;
}

const char* keyboard_key_setting_name(const SDL_Keycode key) {
    if (key == SDLK_UNKNOWN) return "None";
    if (key == SDLK_LSHIFT) return "Left Shift";
    if (key == SDLK_GRAVE) return "Grave";
    return SDL_GetKeyName(key);
}

SDL_Keycode keyboard_key_from_setting(const std::string& value) {
    // Keep human-readable names stable across SDL versions and preserve the
    // legacy Grave shortcut for existing settings files.
    if (value == "Left Shift" || value == "LShift") return SDLK_LSHIFT;
    if (value == "Grave" || value == "Backquote") return SDLK_GRAVE;
    return SDL_GetKeyFromName(value.c_str());
}

const char* gamepad_button_setting_name(const SDL_GamepadButton button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
    case SDL_GAMEPAD_BUTTON_EAST: return "east";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpad_right";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpad_left";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpad_up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpad_down";
    default: return SDL_GetGamepadStringForButton(button);
    }
}

SDL_GamepadButton gamepad_button_from_setting(const std::string& value) {
    if (value == "south") return SDL_GAMEPAD_BUTTON_SOUTH;
    if (value == "east") return SDL_GAMEPAD_BUTTON_EAST;
    if (value == "dpad_right") return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    if (value == "dpad_left") return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    if (value == "dpad_up") return SDL_GAMEPAD_BUTTON_DPAD_UP;
    if (value == "dpad_down") return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    return SDL_GetGamepadButtonFromString(value.c_str());
}

void append_missing_portable_settings(
    const std::filesystem::path& path, const AppSettings& settings,
    const bool has_palette, const std::array<bool, 8>& has_keyboard,
    const std::array<bool, 8>& has_gamepad,
    const std::array<bool, shortcut_names.size()>& has_shortcuts,
    const bool has_video_mode,
    const bool has_touch_scale, const bool has_touch_opacity,
    const std::array<bool, touch_layout_count * touch_control_count>&
        has_touch_positions) {
    const auto complete = has_palette &&
        std::all_of(has_keyboard.begin(), has_keyboard.end(),
                    [](const bool value) { return value; }) &&
        std::all_of(has_gamepad.begin(), has_gamepad.end(),
                    [](const bool value) { return value; }) &&
        std::all_of(has_shortcuts.begin(), has_shortcuts.end(),
                    [](const bool value) { return value; }) &&
        has_video_mode && has_touch_scale && has_touch_opacity &&
        std::all_of(has_touch_positions.begin(), has_touch_positions.end(),
                    [](const bool value) { return value; });
    if (complete) return;
    std::ofstream output(path, std::ios::app);
    if (!output) {
        std::cerr << "Warning: could not complete portable settings file: "
                  << path << '\n';
        return;
    }
    output << "\n# Missing entries added automatically by GBB\n";
    if (!has_palette) {
        output << "palette = "
               << gameboy::display_palettes[settings.palette].id << '\n';
    }
    if (!has_video_mode) {
        output << "video.Mode = "
               << gameboy::video_mode_info(settings.video_mode).id << '\n';
    }
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        if (has_keyboard[index]) continue;
        output << "keyboard." << button_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.keys[index][0]);
        if (settings.bindings.keys[index][1] != SDLK_UNKNOWN) {
            output << ' '
                   << keyboard_key_setting_name(settings.bindings.keys[index][1]);
        }
        output << '\n';
    }
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        if (has_gamepad[index]) continue;
        output << "gamepad." << button_names[index] << " = "
               << gamepad_button_setting_name(
                      settings.bindings.gamepad_buttons[index])
               << '\n';
    }
    for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
        if (has_shortcuts[index]) continue;
        output << "keyboard." << shortcut_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.shortcuts[index])
               << '\n';
    }
    if (!has_touch_scale) {
        output << "touch.Size = " << settings.touch.scale << '\n';
    }
    if (!has_touch_opacity) {
        output << "touch.Opacity = " << settings.touch.opacity << '\n';
    }
    for (std::size_t orientation = 0; orientation < touch_layout_count;
         ++orientation) {
        for (std::size_t control = 0; control < touch_control_count;
             ++control) {
            const auto index = orientation * touch_layout_stride + control * 2;
            const auto entry = orientation * touch_control_count + control;
            if (has_touch_positions[entry]) continue;
            output << "touch." << touch_layout_names[orientation] << '.'
                   << touch_control_names[control] << " = "
                   << settings.touch.positions[index] << ' '
                   << settings.touch.positions[index + 1] << '\n';
        }
    }
}

void write_portable_settings(const std::filesystem::path& preference_directory,
                             const AppSettings& settings) {
    const auto path = portable_settings_path(preference_directory);
    if (path.empty()) return;
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        std::cerr << "Warning: could not write portable settings file: "
                  << path << '\n';
        return;
    }
    output << "# Go Bigger Boy portable settings\n"
              "# Copy this file beside another GBB installation to share "
              "these settings.\n"
              "# Add an optional second keyboard key after the first, for "
              "example: Z Y. Set emulator shortcuts to None to disable "
              "them. Android touch controls use a size multiplier and "
              "opacity between 0 and 1 plus separate portrait and landscape "
              "touch layouts. Video.Mode accepts nearest, bilinear, sharp, "
              "integer, or lcd.\n\n"
              "palette = "
           << gameboy::display_palettes[settings.palette].id << "\n"
              "video.Mode = "
           << gameboy::video_mode_info(settings.video_mode).id << "\n\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        output << "keyboard." << button_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.keys[index][0]);
        if (settings.bindings.keys[index][1] != SDLK_UNKNOWN) {
            output << ' '
                   << keyboard_key_setting_name(settings.bindings.keys[index][1]);
        }
        output << '\n';
    }
    output << '\n';
    for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
        output << "keyboard." << shortcut_names[index] << " = "
               << keyboard_key_setting_name(settings.bindings.shortcuts[index])
               << '\n';
    }
    output << '\n';
    output << "touch.Size = " << settings.touch.scale << '\n';
    output << "touch.Opacity = " << settings.touch.opacity << "\n\n";
    for (std::size_t orientation = 0; orientation < touch_layout_count;
         ++orientation) {
        for (std::size_t control = 0; control < touch_control_count;
             ++control) {
            const auto index = orientation * touch_layout_stride + control * 2;
            output << "touch." << touch_layout_names[orientation] << '.'
                   << touch_control_names[control] << " = "
                   << settings.touch.positions[index] << ' '
                   << settings.touch.positions[index + 1] << '\n';
        }
    }
    output << '\n';
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        output << "gamepad." << button_names[index] << " = "
               << gamepad_button_setting_name(
                      settings.bindings.gamepad_buttons[index])
               << '\n';
    }
}

AppSettings load_portable_settings(
    const std::filesystem::path& preference_directory) {
    AppSettings settings;
    const auto path = portable_settings_path(preference_directory);
    std::ifstream input(path);
    if (!input) {
        settings.bindings = load_legacy_bindings(preference_directory);
        settings.palette = load_legacy_display_palette(preference_directory);
        write_portable_settings(preference_directory, settings);
        return settings;
    }

    auto loaded_keys = settings.bindings.keys;
    auto loaded_buttons = settings.bindings.gamepad_buttons;
    auto loaded_shortcuts = settings.bindings.shortcuts;
    auto loaded_touch_positions = settings.touch.positions;
    std::array<float, 16> legacy_touch_positions{};
    std::array<bool, 8> has_legacy_touch_positions{};
    bool has_palette = false;
    bool has_video_mode = false;
    std::array<bool, 8> has_keyboard{};
    std::array<bool, 8> has_gamepad{};
    std::array<bool, shortcut_names.size()> has_shortcuts{};
    bool has_touch_scale = false;
    bool has_touch_opacity = false;
    std::array<bool, touch_layout_count * touch_control_count>
        has_touch_positions{};
    constexpr std::array<const char*, 8> names{{
        "Right", "Left", "Up", "Down", "A", "B", "Select", "Start"}};
    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find_first_of("#;");
        if (comment != std::string::npos) line.resize(comment);
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = trimmed_setting(line.substr(0, separator));
        const auto value = trimmed_setting(line.substr(separator + 1));
        if (key == "palette") {
            has_palette = true;
            const auto found = std::find_if(
                gameboy::display_palettes.begin(),
                gameboy::display_palettes.end(),
                [&value](const gameboy::DisplayPalette& palette) {
                    return value == palette.id;
                });
            if (found != gameboy::display_palettes.end()) {
                settings.palette = static_cast<std::size_t>(
                    found - gameboy::display_palettes.begin());
            }
            continue;
        }
        if (key == "video.Mode") {
            has_video_mode = true;
            settings.video_mode = gameboy::video_mode_from_id(value);
            continue;
        }
        if (key == "touch.Size") {
            has_touch_scale = true;
            settings.touch.scale = parse_touch_value(
                value, settings.touch.scale, minimum_touch_scale,
                maximum_touch_scale);
            continue;
        }
        if (key == "touch.Opacity") {
            has_touch_opacity = true;
            settings.touch.opacity = parse_touch_value(
                value, settings.touch.opacity, minimum_touch_opacity,
                maximum_touch_opacity);
            continue;
        }
        bool touch_setting = false;
        for (std::size_t orientation = 0; orientation < touch_layout_count;
             ++orientation) {
            for (std::size_t control = 0; control < touch_control_count;
                 ++control) {
                const auto setting_name =
                    std::string("touch.") + touch_layout_names[orientation] +
                    '.' + touch_control_names[control];
                if (key != setting_name) continue;
                touch_setting = true;
                std::istringstream values(value);
                float x = 0.0F;
                float y = 0.0F;
                if (values >> x >> y) {
                    const auto index = orientation * touch_layout_stride +
                                       control * 2;
                    loaded_touch_positions[index] = std::clamp(
                        x, minimum_touch_position, maximum_touch_position);
                    loaded_touch_positions[index + 1] = std::clamp(
                        y, minimum_touch_position, maximum_touch_position);
                    has_touch_positions[orientation * touch_control_count +
                                        control] = true;
                }
            }
        }
        if (touch_setting) continue;
        for (std::size_t index = 0; index < button_names.size(); ++index) {
            if (key != std::string("touch.") + button_names[index]) continue;
            std::istringstream values(value);
            float x = 0.0F;
            float y = 0.0F;
            if (values >> x >> y) {
                legacy_touch_positions[index * 2] = std::clamp(
                    x, minimum_touch_position, maximum_touch_position);
                legacy_touch_positions[index * 2 + 1] = std::clamp(
                    y, minimum_touch_position, maximum_touch_position);
                has_legacy_touch_positions[index] = true;
            }
            break;
        }
        bool shortcut_setting = false;
        for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
            if (key != std::string("keyboard.") + shortcut_names[index]) {
                continue;
            }
            has_shortcuts[index] = true;
            shortcut_setting = true;
            if (value == "None") {
                loaded_shortcuts[index] = SDLK_UNKNOWN;
            } else {
                const auto parsed = keyboard_key_from_setting(value);
                if (parsed != SDLK_UNKNOWN) loaded_shortcuts[index] = parsed;
            }
            break;
        }
        if (shortcut_setting) continue;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (key == std::string("keyboard.") + names[index]) {
                has_keyboard[index] = true;
                std::array<SDL_Keycode, 2> parsed_keys{
                    SDLK_UNKNOWN, SDLK_UNKNOWN};
                bool parsed_mapping = value == "None";
                const auto whole = parsed_mapping
                                       ? SDLK_UNKNOWN
                                       : keyboard_key_from_setting(value);
                if (whole != SDLK_UNKNOWN) {
                    parsed_keys[0] = whole;
                    parsed_mapping = true;
                } else {
                    std::istringstream values(value);
                    std::string name;
                    std::size_t slot = 0;
                    while (slot < parsed_keys.size() && values >> name) {
                        if (name == "None") {
                            ++slot;
                            parsed_mapping = true;
                            continue;
                        }
                        const auto parsed = keyboard_key_from_setting(name);
                        if (parsed != SDLK_UNKNOWN) {
                            parsed_keys[slot++] = parsed;
                            parsed_mapping = true;
                        }
                    }
                }
                if (parsed_mapping) {
                    loaded_keys[index] = parsed_keys;
                }
            } else if (key == std::string("gamepad.") + names[index]) {
                has_gamepad[index] = true;
                const auto parsed = gamepad_button_from_setting(value);
                if (parsed >= 0 && parsed < SDL_GAMEPAD_BUTTON_COUNT) {
                    loaded_buttons[index] = parsed;
                }
            }
        }
    }
    std::vector<SDL_Keycode> unique_keys;
    for (const auto& keys : loaded_keys) {
        for (const auto key : keys) {
            if (key != SDLK_UNKNOWN) unique_keys.push_back(key);
        }
    }
    for (const auto key : loaded_shortcuts) {
        if (key != SDLK_UNKNOWN) unique_keys.push_back(key);
    }
    std::sort(unique_keys.begin(), unique_keys.end());
    if (std::adjacent_find(unique_keys.begin(), unique_keys.end()) ==
        unique_keys.end()) {
        settings.bindings.keys = loaded_keys;
    }
    if (std::adjacent_find(unique_keys.begin(), unique_keys.end()) ==
        unique_keys.end()) {
        settings.bindings.shortcuts = loaded_shortcuts;
    }
    auto unique_buttons = loaded_buttons;
    std::sort(unique_buttons.begin(), unique_buttons.end());
    if (std::adjacent_find(unique_buttons.begin(), unique_buttons.end()) ==
        unique_buttons.end()) {
        settings.bindings.gamepad_buttons = loaded_buttons;
    }
    const auto legacy_position = [&legacy_touch_positions](
                                     const std::size_t index) {
        return std::pair<float, float>{legacy_touch_positions[index * 2],
                                       legacy_touch_positions[index * 2 + 1]};
    };
    const auto legacy_dpad = [&]() {
        float x = 0.0F;
        float y = 0.0F;
        std::size_t count = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            if (!has_legacy_touch_positions[index]) continue;
            x += legacy_touch_positions[index * 2];
            y += legacy_touch_positions[index * 2 + 1];
            ++count;
        }
        return count == 0 ? std::pair<float, float>{0.27F, 0.82F}
                          : std::pair<float, float>{x / count, y / count};
    };
    for (std::size_t orientation = 0; orientation < touch_layout_count;
         ++orientation) {
        const auto dpad = legacy_dpad();
        const auto legacy_controls = std::array<std::pair<float, float>, 5>{
            dpad, legacy_position(4), legacy_position(5),
            legacy_position(6), legacy_position(7)};
        for (std::size_t control = 0; control < touch_control_count;
             ++control) {
            const auto entry = orientation * touch_control_count + control;
            if (has_touch_positions[entry]) continue;
            const auto legacy_index = control == 0 ? 0 : control + 3;
            if (!has_legacy_touch_positions[legacy_index] &&
                (control != 0 ||
                 !std::any_of(has_legacy_touch_positions.begin(),
                              has_legacy_touch_positions.begin() + 4,
                              [](const bool value) { return value; }))) {
                continue;
            }
            const auto index = orientation * touch_layout_stride + control * 2;
            loaded_touch_positions[index] = legacy_controls[control].first;
            loaded_touch_positions[index + 1] = legacy_controls[control].second;
        }
    }
    settings.touch.positions = loaded_touch_positions;
    append_missing_portable_settings(path, settings, has_palette,
                                     has_keyboard, has_gamepad, has_shortcuts,
                                     has_video_mode,
                                     has_touch_scale, has_touch_opacity,
                                     has_touch_positions);
    return settings;
}

AppSettings load_app_settings(
    const std::filesystem::path& preference_directory) {
    return load_portable_settings(preference_directory);
}

void save_app_settings(const std::filesystem::path& preference_directory,
                       const InputBindings& bindings,
                       const std::size_t palette) {
    if (palette >= gameboy::display_palettes.size()) return;
    auto settings = load_app_settings(preference_directory);
    settings.bindings = bindings;
    settings.palette = palette;
    write_portable_settings(preference_directory, settings);
}

gameboy::VideoMode load_video_mode(const std::filesystem::path& directory) {
    return load_app_settings(directory).video_mode;
}

void save_video_mode(const std::filesystem::path& directory,
                     const gameboy::VideoMode mode) {
    auto settings = load_app_settings(directory);
    settings.video_mode = mode;
    write_portable_settings(directory, settings);
}

TouchControlSettings load_touch_control_settings(
    const std::filesystem::path& directory) {
    return load_app_settings(directory).touch;
}

void save_touch_control_settings(const std::filesystem::path& directory,
                                 const float scale, const float opacity) {
    auto settings = load_app_settings(directory);
    settings.touch.scale = std::clamp(scale, minimum_touch_scale,
                                      maximum_touch_scale);
    settings.touch.opacity = std::clamp(opacity, minimum_touch_opacity,
                                        maximum_touch_opacity);
    write_portable_settings(directory, settings);
}

std::array<float, touch_layout_count * touch_layout_stride>
load_touch_control_layout(
    const std::filesystem::path& directory) {
    return load_app_settings(directory).touch.positions;
}

void save_touch_control_layout(const std::filesystem::path& directory,
                               const std::array<float,
                                                touch_layout_count *
                                                    touch_layout_stride>&
                                   positions) {
    auto settings = load_app_settings(directory);
    for (std::size_t index = 0; index < positions.size(); ++index) {
        settings.touch.positions[index] = std::clamp(
            positions[index], minimum_touch_position, maximum_touch_position);
    }
    write_portable_settings(directory, settings);
}

InputBindings load_bindings(const std::filesystem::path& directory) {
    return load_app_settings(directory).bindings;
}

std::size_t load_display_palette(const std::filesystem::path& directory) {
    return load_app_settings(directory).palette;
}

void save_bindings(const std::filesystem::path& directory,
                   const InputBindings& bindings) {
    save_app_settings(directory, bindings, load_display_palette(directory));
}

void save_display_palette(const std::filesystem::path& directory,
                          const std::size_t palette) {
    save_app_settings(directory, load_bindings(directory), palette);
}

std::filesystem::path quick_state_path(
    const std::filesystem::path& preference_path,
    const gameboy::Emulator& emulator) {
    if (preference_path.empty()) {
        throw std::runtime_error("Could not locate the preferences directory");
    }
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0')
         << emulator.rom_fingerprint() << ".gbbs";
    return preference_path / "states" / name.str();
}

void replace_file_atomically(const std::filesystem::path& temporary,
                             const std::filesystem::path& destination) {
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Could not publish quick save");
    }
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "Could not publish quick save");
    }
#endif
}

void save_printer_bitmap(const std::filesystem::path& path,
                         const gameboy::PrinterImage& image) {
    const auto bytes = gameboy::encode_printer_bmp(image);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Could not create printer image: " +
                                 path.u8string());
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("Could not write printer image: " +
                                 path.u8string());
    }
}

void save_completed_prints(gameboy::Emulator* emulator, SDL_Window* window,
                           const std::filesystem::path& preference_path,
                           const std::string& current_rom,
                           std::uint64_t& print_sequence) {
    if (emulator == nullptr) return;
    auto images = emulator->bus().take_printer_images();
    if (images.empty()) return;

    const auto directory = preference_path.empty()
                               ? std::filesystem::current_path() / "GBB Prints"
                               : preference_path / "prints";
    std::filesystem::create_directories(directory);
    auto rom_name = std::filesystem::u8path(current_rom).stem().u8string();
    if (rom_name.empty()) rom_name = "gameboy";
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now()
                                   .time_since_epoch())
                               .count();
    std::vector<std::filesystem::path> paths;
    paths.reserve(images.size());
    for (const auto& image : images) {
        const auto filename = rom_name + "-print-" +
                              std::to_string(timestamp) + "-" +
                              std::to_string(++print_sequence) + ".bmp";
        auto path = directory / std::filesystem::u8path(filename);
        save_printer_bitmap(path, image);
        paths.push_back(std::move(path));
    }

    std::ostringstream message;
    message << "Saved " << paths.size() << " printer image";
    if (paths.size() != 1) message << 's';
    message << " to:\n" << directory.u8string();
    const auto text = message.str();
    std::cerr << text << '\n';
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Game Boy Printer", text.c_str(), window));
}

void save_quick_state(const std::filesystem::path& preference_path,
                      const gameboy::Emulator& emulator) {
    const auto path = quick_state_path(preference_path, emulator);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    try {
        const auto state = emulator.save_state();
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("Could not open temporary quick-save file");
        }
        output.write(reinterpret_cast<const char*>(state.data()),
                     static_cast<std::streamsize>(state.size()));
        output.flush();
        if (!output) throw std::runtime_error("Could not write quick save");
        output.close();
        replace_file_atomically(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void load_quick_state(const std::filesystem::path& preference_path,
                      gameboy::Emulator& emulator) {
    const auto path = quick_state_path(preference_path, emulator);
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (!size_error && size > maximum_quick_state_size) {
        throw std::runtime_error("Quick save is too large");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("No quick save exists for this ROM");
    std::vector<std::uint8_t> state(
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
    if (input.bad()) throw std::runtime_error("Could not read quick save");
    emulator.load_state(state);
}

void SDLCALL file_dialog_callback(void* userdata,
                                  const char* const* filelist, int) {
    auto& state = *static_cast<DialogState*>(userdata);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.active = false;
    if (filelist == nullptr) {
        state.error = SDL_GetError();
    } else if (filelist[0] != nullptr) {
        state.selected_path = filelist[0];
    }
}

void show_rom_dialog(DialogState& state, SDL_Window* window) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.active) return;
        state.active = true;
    }
    static constexpr SDL_DialogFileFilter filters[] = {
        {"Game Boy ROMs", "gb;gbc"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(file_dialog_callback, &state, window, filters,
                           static_cast<int>(std::size(filters)), nullptr, false);
}

bool dialog_active(DialogState& state) {
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.active;
}

void collect_dialog_result(DialogState& state,
                           std::optional<std::string>& path,
                           std::optional<std::string>& error) {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.selected_path) {
        path = std::move(*state.selected_path);
        state.selected_path.reset();
    }
    if (state.error) {
        error = std::move(*state.error);
        state.error.reset();
    }
}

std::optional<gameboy::Button> keyboard_button(const InputBindings& bindings,
                                               const SDL_Keycode key) {
    for (std::size_t index = 0; index < bindings.keys.size(); ++index) {
        if (std::find(bindings.keys[index].begin(), bindings.keys[index].end(),
                      key) != bindings.keys[index].end()) {
            return button_order[index];
        }
    }
    return std::nullopt;
}

std::optional<gameboy::Button> gamepad_button(const InputBindings& bindings,
                                              const Uint8 button) {
    for (std::size_t index = 0; index < bindings.gamepad_buttons.size(); ++index) {
        if (bindings.gamepad_buttons[index] ==
            static_cast<SDL_GamepadButton>(button)) {
            return button_order[index];
        }
    }
    return std::nullopt;
}

bool shortcut_pressed(const InputBindings& bindings, const std::size_t shortcut,
                      const SDL_Keycode key) {
    return shortcut < bindings.shortcuts.size() &&
           bindings.shortcuts[shortcut] != SDLK_UNKNOWN &&
           bindings.shortcuts[shortcut] == key;
}

#ifdef __ANDROID__
bool touch_is_landscape(const SdlResources& sdl) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    return width >= height;
}

std::size_t touch_layout_offset(const SdlResources& sdl) {
    return (touch_is_landscape(sdl) ? 1U : 0U) * touch_layout_stride;
}

float touch_game_scale(const SdlResources& sdl) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    return std::min(static_cast<float>(width) /
                        static_cast<float>(gameboy::Ppu::screen_width),
                    static_cast<float>(height) /
                        static_cast<float>(gameboy::Ppu::screen_height));
}

std::pair<float, float> touch_control_position(const SdlResources& sdl,
                                               const std::size_t control) {
    const auto index = touch_layout_offset(sdl) + control * 2;
    return {sdl.touch_settings.positions[index],
            sdl.touch_settings.positions[index + 1]};
}

std::optional<std::size_t> touch_button_index(const float x, const float y,
                                              const SdlResources& sdl) {
    const auto scale = std::clamp(sdl.touch_settings.scale,
                                  minimum_touch_scale, maximum_touch_scale);
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    const auto pixel_x = x * static_cast<float>(width);
    const auto pixel_y = y * static_cast<float>(height);
    constexpr std::array<float, 4> widths{{42.0F, 24.0F, 24.0F, 22.0F}};
    constexpr std::array<float, 4> heights{{42.0F, 24.0F, 24.0F, 10.0F}};
    const auto inside = [&](const std::size_t control) {
        const auto [normalized_x, normalized_y] =
            touch_control_position(sdl, control);
        const auto center_x = normalized_x * static_cast<float>(width);
        const auto center_y = normalized_y * static_cast<float>(height);
        const auto dx = pixel_x - center_x;
        const auto dy = pixel_y - center_y;
        const auto size = touch_game_scale(sdl) * scale;
        if (control == 0) {
            return std::abs(dx) <= widths[0] * size * 0.5F &&
                   std::abs(dy) <= heights[0] * size * 0.5F;
        }
        if (control == 1 || control == 2) {
            const auto radius = widths[control] * size * 0.5F;
            return dx * dx + dy * dy <= radius * radius;
        }
        return std::abs(dx) <= widths[control] * size * 0.5F &&
               std::abs(dy) <= heights[control] * size * 0.5F;
    };
    // Check the face and system buttons before the D-pad if a custom layout
    // intentionally places controls near one another.
    for (const auto control : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                               std::size_t{4}}) {
        if (inside(control)) return control + 3;
    }
    if (inside(0)) {
        const auto [normalized_x, normalized_y] = touch_control_position(sdl, 0);
        const auto dx = pixel_x - normalized_x * static_cast<float>(width);
        const auto dy = pixel_y - normalized_y * static_cast<float>(height);
        return std::abs(dx) >= std::abs(dy) ? (dx >= 0.0F ? 0U : 1U)
                                            : (dy >= 0.0F ? 3U : 2U);
    }
    return std::nullopt;
}

void refresh_touch_buttons(gameboy::Emulator* emulator, SdlResources& sdl) {
    std::array<bool, 8> pressed{};
    for (const auto& touch : sdl.touches) {
        if (const auto index = touch_button_index(touch.x, touch.y, sdl)) {
            pressed[*index] = true;
        }
    }
    if (emulator != nullptr) {
        for (std::size_t index = 0; index < pressed.size(); ++index) {
            if (pressed[index] != sdl.touch_buttons[index]) {
                emulator->set_button(button_order[index], pressed[index]);
            }
        }
    }
    sdl.touch_buttons = pressed;
}

void clear_touch_buttons(gameboy::Emulator* emulator, SdlResources& sdl) {
    sdl.touches.clear();
    if (emulator != nullptr) {
        for (std::size_t index = 0; index < sdl.touch_buttons.size(); ++index) {
            if (sdl.touch_buttons[index]) {
                emulator->set_button(button_order[index], false);
            }
        }
    }
    sdl.touch_buttons.fill(false);
}

std::pair<float, float> logical_touch_position(const SDL_TouchFingerEvent& event,
                                                SdlResources& sdl) {
    int width = 1;
    int height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &width, &height));
    auto x = event.x * static_cast<float>(width);
    auto y = event.y * static_cast<float>(height);
    static_cast<void>(
        SDL_RenderCoordinatesFromWindow(sdl.renderer, x, y, &x, &y));
    return {x / static_cast<float>(gameboy::Ppu::screen_width),
            y / static_cast<float>(gameboy::Ppu::screen_height)};
}

std::pair<float, float> window_touch_position(
    const SDL_TouchFingerEvent& event) {
    return {event.x, event.y};
}
#endif

bool reserved_gameplay_key(const InputBindings& bindings,
                           const SDL_Keycode key) {
    switch (key) {
    case SDLK_ESCAPE:
    case SDLK_SPACE:
    case SDLK_F1:
    case SDLK_F11:
        return true;
    default:
        return std::find(bindings.shortcuts.begin(), bindings.shortcuts.end(),
                         key) != bindings.shortcuts.end();
    }
}

void release_all_buttons(gameboy::Emulator& emulator) {
    for (const auto button : button_order) emulator.set_button(button, false);
}

void stop_rumble(SdlResources& sdl) noexcept {
    if (sdl.gamepad != nullptr && sdl.rumble_output_active) {
        static_cast<void>(SDL_RumbleGamepad(sdl.gamepad, 0, 0, 0));
    }
    sdl.rumble_output_active = false;
    sdl.rumble_refresh = {};
}

void update_rumble(const gameboy::Emulator* emulator, SdlResources& sdl,
                   const bool enabled) {
    const auto desired = enabled && emulator != nullptr &&
                         emulator->has_rumble() && emulator->rumble_active();
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
            std::cerr << "Warning: the connected gamepad does not provide "
                         "rumble output: "
                      << SDL_GetError() << '\n';
            sdl.rumble_warning_shown = true;
        }
    }
}

void update_window_title(SDL_Window* window, const std::string& current_rom,
                         const bool paused,
                         const std::optional<BindingConfiguration>& configuring) {
    std::string title = "Go Bigger Boy (GBB)";
    if (configuring) {
        title += configuring->device == BindingDevice::keyboard
                     ? " - Press a key for "
                     : " - Press a gamepad button for ";
        title += button_names[configuring->index];
        if (configuring->device == BindingDevice::keyboard) {
            title += configuring->slot == 0
                         ? " (primary)"
                         : " (secondary; Space: none)";
        }
        title += " (Esc: cancel)";
    } else {
        if (!current_rom.empty()) {
            title += " - " + rom_filename(current_rom);
        } else {
            title += " - Drop a ROM here or press Ctrl+O";
        }
        if (paused) title += " [PAUSED]";
        title += "  (F1: Help)";
    }
    if (!SDL_SetWindowTitle(window, title.c_str())) {
        sdl_error("Could not update window title");
    }
}

enum class ControlsAction { cancel, keyboard, gamepad, reset };

void begin_binding_configuration(
    InputBindings& bindings, InputBindings& backup,
    std::optional<BindingConfiguration>& configuring,
    const BindingDevice device) {
    backup = bindings;
    if (device == BindingDevice::keyboard) {
        for (auto& keys : bindings.keys) keys.fill(SDLK_UNKNOWN);
    }
    configuring = BindingConfiguration{device, 0, 0};
}

ControlsAction show_controls_dialog(SDL_Window* window,
                                    const InputBindings& bindings) {
    std::ostringstream message;
    message << "Current controls:\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        message << button_names[index] << ": "
                << SDL_GetKeyName(bindings.keys[index][0]);
        if (bindings.keys[index][1] != SDLK_UNKNOWN) {
            message << " or " << SDL_GetKeyName(bindings.keys[index][1]);
        }
        message << " / "
                << SDL_GetGamepadStringForButton(
                       bindings.gamepad_buttons[index])
                << '\n';
    }
    message << "\nChoose which controls to configure. Keyboard setup asks "
               "for a primary and optional secondary key; press Space to "
               "skip a secondary key.";

    constexpr std::array<SDL_MessageBoxButtonData, 4> buttons{{
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {0, 1, "Keyboard"},
        {0, 2, "Gamepad"},
        {0, 3, "Restore defaults"},
    }};
    const auto text = message.str();
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Configure controls", text.c_str(),
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = 0;
    if (!SDL_ShowMessageBox(&box, &selection)) return ControlsAction::cancel;
    switch (selection) {
    case 1: return ControlsAction::keyboard;
    case 2: return ControlsAction::gamepad;
    case 3: return ControlsAction::reset;
    default: return ControlsAction::cancel;
    }
}

std::optional<std::size_t> show_palette_dialog(SDL_Window* window,
                                               const std::size_t current) {
    std::array<SDL_MessageBoxButtonData,
               gameboy::display_palettes.size() + 1>
        buttons{};
    for (std::size_t index = 0; index < gameboy::display_palettes.size();
         ++index) {
        buttons[index] = {0, static_cast<int>(index),
                          gameboy::display_palettes[index].name};
    }
    buttons.back() = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, "Cancel"};

    const auto current_name = current < gameboy::display_palettes.size()
                                  ? gameboy::display_palettes[current].name
                                  : gameboy::display_palettes.front().name;
    const auto message = std::string("Current palette: ") + current_name +
                         "\n\nChoose a display palette:";
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Display palette", message.c_str(),
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = -1;
    if (!SDL_ShowMessageBox(&box, &selection) || selection < 0 ||
        static_cast<std::size_t>(selection) >=
            gameboy::display_palettes.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(selection);
}

std::optional<gameboy::VideoMode> show_video_dialog(
    SDL_Window* window, const gameboy::VideoMode current) {
    std::array<SDL_MessageBoxButtonData, gameboy::video_modes.size() + 1>
        buttons{};
    for (std::size_t index = 0; index < gameboy::video_modes.size(); ++index) {
        buttons[index] = {0, static_cast<int>(index),
                          gameboy::video_modes[index].name.data()};
    }
    buttons.back() = {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, -1, "Cancel"};
    const auto message = std::string("Current pipeline: ") +
                         std::string{gameboy::video_mode_info(current).name} +
                         "\n\nChoose a presentation mode:";
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION, window, "Video pipeline", message.c_str(),
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = -1;
    if (!SDL_ShowMessageBox(&box, &selection) || selection < 0 ||
        static_cast<std::size_t>(selection) >= gameboy::video_modes.size()) {
        return std::nullopt;
    }
    return gameboy::video_modes[static_cast<std::size_t>(selection)].mode;
}

void show_error(SDL_Window* window, const std::string& message);

void choose_video_mode(SdlResources& sdl,
                       const std::filesystem::path& preference_path) {
    const auto selected = show_video_dialog(sdl.window, sdl.video_mode);
    if (!selected) return;
    if (!configure_video_pipeline(sdl, *selected)) {
        show_error(sdl.window, "Could not configure the selected video pipeline.");
        return;
    }
    save_video_mode(preference_path, *selected);
}

void choose_display_palette(gameboy::Emulator* emulator, SdlResources& sdl,
                            const std::filesystem::path& preference_path,
                            std::size_t& display_palette) {
    if (emulator != nullptr) release_all_buttons(*emulator);
    const auto selected = show_palette_dialog(sdl.window, display_palette);
    if (!selected) return;
    display_palette = *selected;
    if (emulator != nullptr) {
        emulator->set_dmg_compatibility_colors(
            gameboy::display_palettes[display_palette].cgb_compatibility);
    }
    save_display_palette(preference_path, display_palette);
}

bool confirm_exit(SDL_Window* window) {
    constexpr std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Exit"},
    }};
    constexpr auto message =
        "Are you sure you want to close Go Bigger Boy?";
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_WARNING, window, "Exit Go Bigger Boy?", message,
        static_cast<int>(buttons.size()), buttons.data(), nullptr,
    };
    auto selection = 0;
    return SDL_ShowMessageBox(&box, &selection) && selection == 1;
}

void show_help(SDL_Window* window, const InputBindings& bindings) {
    std::ostringstream message;
    message << "Version " GBB_VERSION "\n\nGAMEPLAY CONTROLS\n";
    for (std::size_t index = 0; index < button_names.size(); ++index) {
        message << button_names[index] << ": "
                << keyboard_key_setting_name(bindings.keys[index][0]);
        if (bindings.keys[index][1] != SDLK_UNKNOWN) {
            message << " / "
                    << keyboard_key_setting_name(bindings.keys[index][1]);
        }
        message << '\n';
    }
    message << "\nCONFIGURABLE EMULATOR SHORTCUTS\n";
    for (std::size_t index = 0; index < shortcut_names.size(); ++index) {
        message << shortcut_names[index] << ": "
                << keyboard_key_setting_name(bindings.shortcuts[index]) << '\n';
    }
    message <<
        "\nGENERAL\n"
        "Space: Pause/resume\n"
        "Ctrl+R: Reset\n"
        "Ctrl+O: Open ROM\n"
        "Ctrl+L: Open game library\n"
        "Ctrl+K: Configure controls\n"
        "Ctrl+P: Choose display palette\n"
        "Ctrl+G: Open GameShark cheat manager\n"
        "Game library: Choose the video pipeline\n"
        "Ctrl+1 through Ctrl+9: Open recent ROM\n"
        "Configured SaveState key: Save state\n"
        "Configured LoadState key: Load state\n"
        "Configured FastForward key: Hold for 4x speed\n"
        "Configured Rewind key: Hold to rewind\n"
        "F12: Open/close debugger\n"
        "Debugger F5: Run/pause\n"
        "Debugger F6: Start/stop input recording\n"
        "Debugger F7: Replay the latest recording\n"
        "Debugger F8: Open the TAS frame editor\n"
        "Debugger F9: Open the live sprite editor\n"
        "Debugger F10: Step one instruction\n"
        "Debugger F11: Step one frame\n"
        "\nTAS EDITOR\n"
        "Up/Down: Select frame; Insert/Delete/End: Edit timeline\n"
        "Ctrl+N: New from current state; Ctrl+S: Save; F7: Run\n"
        "\nSPRITE EDITOR\n"
        "1-4: Color; Left/right mouse: Paint/erase\n"
        "Ctrl+Z: Undo; Delete: Clear; B: Switch CGB bank\n"
        "Ctrl+S: Save tile patch; Ctrl+O: Import; Ctrl+E: Export IPS\n"
        "\nGAMESHARK CHEAT MANAGER\n"
        "Ctrl+G: Open for the current ROM\n"
        "Space: Toggle selected cheat; Delete: Remove selected cheat\n"
        "Fetch for ROM: Import matching Libretro archive entries\n"
        "F11: Toggle fullscreen\n"
        "F1: Show this help\n"
        "Escape: Quit\n\n"
        "Game Boy Printer pages are saved automatically as BMP images.\n"
        "Game Boy Camera cartridges use the first available webcam.\n"
        "Rumble cartridges vibrate the connected gamepad when supported.";
    const auto text = message.str();
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "Go Bigger Boy (GBB) controls",
        text.c_str(), window));
}

void show_about(SDL_Window* window) {
#ifdef _WIN32
    const auto owner = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    std::wstring message = L"Go Bigger Boy (GBB) v";
    for (const auto character : std::string_view{GBB_VERSION}) {
        message.push_back(static_cast<wchar_t>(
            static_cast<unsigned char>(character)));
    }
    message += L"\n\nA portable Game Boy and Game Boy Color emulator.";
    MSGBOXPARAMSW parameters{};
    parameters.cbSize = sizeof(parameters);
    parameters.hwndOwner = owner;
    parameters.hInstance = GetModuleHandleW(nullptr);
    parameters.lpszText = message.c_str();
    parameters.lpszCaption = L"About Go Bigger Boy";
    parameters.dwStyle = MB_OK | MB_USERICON | MB_SETFOREGROUND;
    parameters.lpszIcon = MAKEINTRESOURCEW(IDI_GBB_ICON);
    static_cast<void>(MessageBoxIndirectW(&parameters));
#else
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_INFORMATION, "About Go Bigger Boy",
        "Go Bigger Boy (GBB) v" GBB_VERSION
        "\n\nA portable Game Boy and Game Boy Color emulator.", window));
#endif
}

void show_error(SDL_Window* window, const std::string& message);
#ifdef __ANDROID__
void open_android_library() noexcept;
void leave_android_game(
    std::unique_ptr<gameboy::Emulator>& emulator, SdlResources& sdl,
    bool& dashboard_visible, bool& paused, bool& fast_forward, bool& rewind,
    RewindHistory& rewind_history, bool& running);
#endif

void activate_dashboard_selection(
    const std::size_t selection, const std::vector<std::string>& recent,
    const InputBindings& bindings, gameboy::Emulator* emulator,
    DialogState& dialog, SdlResources& sdl,
    const std::filesystem::path& preference_path,
    std::optional<std::string>& pending_rom, bool& dashboard_visible,
    std::size_t& display_palette, bool& running) {
    const auto items = dashboard_items(emulator != nullptr, recent);
    if (selection >= items.size()) return;
    const auto& item = items[selection];
    switch (item.action) {
    case DashboardAction::resume:
        dashboard_visible = false;
        break;
    case DashboardAction::open_rom:
        show_rom_dialog(dialog, sdl.window);
        break;
    case DashboardAction::palette:
        choose_display_palette(emulator, sdl, preference_path,
                               display_palette);
        break;
    case DashboardAction::video:
        choose_video_mode(sdl, preference_path);
        break;
    case DashboardAction::shortcuts:
        show_help(sdl.window, bindings);
        break;
    case DashboardAction::recent_rom:
        if (item.recent_index < recent.size()) {
            pending_rom = recent[item.recent_index];
        }
        break;
    case DashboardAction::quit:
        if (confirm_exit(sdl.window)) running = false;
        break;
    }
}

std::optional<std::size_t> dashboard_row_at(
    const float logical_x, const float logical_y, const std::size_t selection,
    const std::size_t item_count) {
    if (logical_x < 9.0F || logical_x > 151.0F ||
        logical_y < dashboard_first_row_y) {
        return std::nullopt;
    }
    const auto row = static_cast<std::size_t>(
        (logical_y - dashboard_first_row_y) / dashboard_row_height);
    if (row >= dashboard_visible_rows) return std::nullopt;
    const auto index = dashboard_first_visible(selection, item_count) + row;
    return index < item_count ? std::optional<std::size_t>{index}
                              : std::nullopt;
}

void process_events(std::unique_ptr<gameboy::Emulator>& emulator,
                    SdlResources& sdl, DialogState& dialog,
                    const std::filesystem::path& preference_path,
                    InputBindings& bindings,
                    InputBindings& configuration_backup,
                    const std::vector<std::string>& recent,
                    const std::string& current_rom,
                    std::optional<BindingConfiguration>& configuring,
                    std::optional<std::string>& pending_rom,
                    std::size_t& display_palette, bool& dashboard_visible,
                    std::size_t& dashboard_selection, bool& paused,
                    bool& fullscreen, bool& fast_forward, bool& rewind,
                    RewindHistory& rewind_history, bool& reset_requested,
                    bool& running
#ifndef __ANDROID__
                    , DesktopDebugger& debugger, InputMovie& input_movie,
                    TasEditor& tas_editor, SpriteEditor& sprite_editor,
                    CheatManager& cheat_manager
#ifdef _WIN32
                    , DesktopMenuBar& desktop_menu
#endif
#endif
                    ) {
#ifndef __ANDROID__
    const auto replaying_input = input_movie.replaying();
    const auto input_movie_active = input_movie.mode() != InputMovie::Mode::idle;
#else
    constexpr auto replaying_input = false;
    constexpr auto input_movie_active = false;
#endif
#ifdef __ANDROID__
    // The Java activity intercepts Android's back callback and sets this
    // flag.  Handle it here, on SDL's thread, rather than allowing the
    // activity to finish while the emulator is still writing its save file.
    if (android_back_requested.exchange(false)) {
        if (dashboard_visible && emulator != nullptr) {
            dashboard_visible = false;
        } else {
            leave_android_game(emulator, sdl, dashboard_visible, paused,
                               fast_forward, rewind, rewind_history, running);
        }
    }
#endif
#ifdef _WIN32
    desktop_menu.update(emulator != nullptr, paused, fullscreen,
                        input_movie.recording(), display_palette, sdl.video_mode);
    const auto menu_command = desktop_menu.take_command();
    const auto menu_value = static_cast<int>(menu_command);
    const auto palette_first =
        static_cast<int>(DesktopMenuCommand::palette_first);
    const auto video_first = static_cast<int>(DesktopMenuCommand::video_first);
    if (menu_value >= palette_first &&
        menu_value < palette_first +
                         static_cast<int>(gameboy::display_palettes.size())) {
        display_palette = static_cast<std::size_t>(menu_value - palette_first);
        save_display_palette(preference_path, display_palette);
        if (emulator != nullptr) {
            emulator->set_dmg_compatibility_colors(
                gameboy::display_palettes[display_palette].cgb_compatibility);
        }
    } else if (menu_value >= video_first &&
               menu_value < video_first +
                                static_cast<int>(gameboy::video_modes.size())) {
        sdl.video_mode = gameboy::video_modes[
            static_cast<std::size_t>(menu_value - video_first)].mode;
        save_video_mode(preference_path, sdl.video_mode);
        if (!configure_video_pipeline(sdl, sdl.video_mode)) {
            show_error(sdl.window, "Could not configure the selected video pipeline.");
        }
    } else {
        switch (menu_command) {
        case DesktopMenuCommand::open_rom:
            show_rom_dialog(dialog, sdl.window);
            break;
        case DesktopMenuCommand::library:
            if (emulator) release_all_buttons(*emulator);
            SDL_HideWindow(sdl.window);
            dashboard_visible = true;
            dashboard_selection = 0;
            break;
        case DesktopMenuCommand::save_state:
            if (emulator) {
                try {
                    save_quick_state(preference_path, *emulator);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Save state", "State saved.",
                        sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            break;
        case DesktopMenuCommand::load_state:
            if (emulator) {
                try {
                    load_quick_state(preference_path, *emulator);
                    rewind_history.clear();
                    release_all_buttons(*emulator);
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            break;
        case DesktopMenuCommand::exit_app:
            if (confirm_exit(sdl.window)) running = false;
            break;
        case DesktopMenuCommand::pause:
            if (emulator) {
                paused = !paused;
                input_movie.release_all(*emulator);
                update_window_title(sdl.window, current_rom, paused, configuring);
            }
            break;
        case DesktopMenuCommand::reset:
            if (emulator && !input_movie_active) reset_requested = true;
            break;
        case DesktopMenuCommand::fullscreen:
            fullscreen = !fullscreen;
            if (!SDL_SetWindowFullscreen(sdl.window, fullscreen)) {
                fullscreen = !fullscreen;
                show_error(sdl.window, SDL_GetError());
            }
            break;
        case DesktopMenuCommand::controls: {
            if (emulator) release_all_buttons(*emulator);
            const auto action = show_controls_dialog(sdl.window, bindings);
            if (action == ControlsAction::reset) {
                bindings = InputBindings{};
                save_bindings(preference_path, bindings);
            } else if (action == ControlsAction::keyboard ||
                       action == ControlsAction::gamepad) {
                if (action == ControlsAction::gamepad && sdl.gamepad == nullptr) {
                    show_error(sdl.window,
                               "Connect a gamepad before configuring it.");
                } else {
                    begin_binding_configuration(
                        bindings, configuration_backup, configuring,
                        action == ControlsAction::keyboard
                            ? BindingDevice::keyboard
                            : BindingDevice::gamepad);
                }
            }
            update_window_title(sdl.window, current_rom, paused, configuring);
            break;
        }
        case DesktopMenuCommand::gameshark:
            if (emulator) {
                release_all_buttons(*emulator);
                cheat_manager.open(sdl.window);
            }
            break;
        case DesktopMenuCommand::debugger:
            if (emulator) {
                release_all_buttons(*emulator);
                debugger.toggle(sdl.window);
            }
            break;
        case DesktopMenuCommand::record_input:
            if (emulator) debugger.request_record_toggle();
            break;
        case DesktopMenuCommand::replay_input:
            if (emulator) debugger.request_replay();
            break;
        case DesktopMenuCommand::tas_editor:
            if (emulator) debugger.request_tas_editor();
            break;
        case DesktopMenuCommand::sprite_editor:
            if (emulator) debugger.request_sprite_editor();
            break;
        case DesktopMenuCommand::shortcuts:
            if (emulator) release_all_buttons(*emulator);
            show_help(sdl.window, bindings);
            break;
        case DesktopMenuCommand::about:
            show_about(sdl.window);
            break;
        default: break;
        }
    }
#endif
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifndef __ANDROID__
        if (cheat_manager.handle_event(event)) continue;
        if (sprite_editor.handle_event(event, emulator.get())) continue;
        if (tas_editor.handle_event(event)) continue;
        if (debugger.handle_event(event, emulator.get())) continue;
#endif
        switch (event.type) {
        case SDL_EVENT_QUIT:
            if (confirm_exit(sdl.window)) running = false;
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data != nullptr) pending_rom = event.drop.data;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
#ifdef __ANDROID__
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                event.key.key == SDLK_AC_BACK) {
                if (dashboard_visible && emulator != nullptr) {
                    dashboard_visible = false;
                } else {
                    leave_android_game(emulator, sdl, dashboard_visible, paused,
                                       fast_forward, rewind, rewind_history,
                                       running);
                }
                break;
            }
#endif
            if (dashboard_visible) {
                if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) break;
                const auto item_count =
                    dashboard_items(emulator != nullptr, recent).size();
                dashboard_selection =
                    std::min(dashboard_selection, item_count - 1);
                if (event.key.key == SDLK_UP) {
                    dashboard_selection = dashboard_selection == 0
                                              ? item_count - 1
                                              : dashboard_selection - 1;
                } else if (event.key.key == SDLK_DOWN) {
                    dashboard_selection =
                        (dashboard_selection + 1) % item_count;
                } else if (event.key.key == SDLK_RETURN ||
                           event.key.key == SDLK_SPACE) {
                    activate_dashboard_selection(
                        dashboard_selection, recent, bindings, emulator.get(), dialog,
                        sdl, preference_path, pending_rom, dashboard_visible,
                        display_palette, running);
                } else if (event.key.key == SDLK_F1) {
                    show_help(sdl.window, bindings);
                } else if (event.key.key == SDLK_O) {
                    show_rom_dialog(dialog, sdl.window);
                } else if (event.key.key == SDLK_ESCAPE) {
                    if (emulator) {
                        dashboard_visible = false;
                    } else if (confirm_exit(sdl.window)) {
                        running = false;
                    }
                }
                break;
            }
            if (configuring) {
                if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) break;
                if (event.key.key == SDLK_ESCAPE) {
                    bindings = configuration_backup;
                    configuring.reset();
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                    break;
                }
                if (configuring->device != BindingDevice::keyboard) break;
                if (configuring->slot == 1 && event.key.key == SDLK_SPACE) {
                    bindings.keys[configuring->index][1] = SDLK_UNKNOWN;
                    configuring->slot = 0;
                    ++configuring->index;
                    if (configuring->index == bindings.keys.size()) {
                        configuring.reset();
                        save_bindings(preference_path, bindings);
                        static_cast<void>(SDL_ShowSimpleMessageBox(
                            SDL_MESSAGEBOX_INFORMATION, "Keyboard controls",
                            "Keyboard bindings saved.", sdl.window));
                    }
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                    break;
                }
                if (reserved_gameplay_key(bindings, event.key.key)) {
                    show_error(sdl.window,
                               "That key is reserved for an emulator shortcut.");
                    break;
                }
                bool duplicate = false;
                for (std::size_t index = 0; index < bindings.keys.size(); ++index) {
                    for (std::size_t slot = 0;
                         slot < bindings.keys[index].size(); ++slot) {
                        if (bindings.keys[index][slot] == event.key.key &&
                            (index != configuring->index ||
                             slot != configuring->slot)) {
                            duplicate = true;
                        }
                    }
                }
                if (duplicate) {
                    show_error(sdl.window, "That key is already assigned.");
                    break;
                }
                bindings.keys[configuring->index][configuring->slot] =
                    event.key.key;
                if (configuring->slot == 0) {
                    configuring->slot = 1;
                } else {
                    configuring->slot = 0;
                    ++configuring->index;
                    if (configuring->index == bindings.keys.size()) {
                        configuring.reset();
                        save_bindings(preference_path, bindings);
                        static_cast<void>(SDL_ShowSimpleMessageBox(
                            SDL_MESSAGEBOX_INFORMATION, "Keyboard controls",
                            "Keyboard bindings saved.", sdl.window));
                    }
                }
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
                break;
            }

            if (!replaying_input && emulator &&
                shortcut_pressed(bindings, shortcut_fast_forward,
                                             event.key.key)) {
                fast_forward = event.type == SDL_EVENT_KEY_DOWN;
                break;
            } else if (!input_movie_active && emulator &&
                       shortcut_pressed(bindings, shortcut_rewind,
                                                    event.key.key)) {
                rewind = event.type == SDL_EVENT_KEY_DOWN;
                break;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == bindings.shortcuts[shortcut_save_state] &&
                       emulator && !replaying_input) {
                try {
                    save_quick_state(preference_path, *emulator);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Save state",
                        "State saved.", sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == bindings.shortcuts[shortcut_load_state] &&
                       emulator && !input_movie_active) {
                try {
                    load_quick_state(preference_path, *emulator);
                    rewind_history.clear();
                    release_all_buttons(*emulator);
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Load state",
                        "State loaded.", sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_O && (event.key.mod & SDL_KMOD_CTRL) != 0) {
                show_rom_dialog(dialog, sdl.window);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_L &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (emulator) release_all_buttons(*emulator);
#ifdef __ANDROID__
                open_android_library();
#else
#ifdef _WIN32
                SDL_HideWindow(sdl.window);
#endif
                dashboard_visible = true;
                dashboard_selection = 0;
#endif
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_K &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                if (emulator) release_all_buttons(*emulator);
                const auto action = show_controls_dialog(sdl.window, bindings);
                if (action == ControlsAction::reset) {
                    bindings = InputBindings{};
                    save_bindings(preference_path, bindings);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Controls",
                        "Keyboard and gamepad bindings restored to defaults.",
                        sdl.window));
                } else if (action == ControlsAction::keyboard ||
                           action == ControlsAction::gamepad) {
                    if (action == ControlsAction::gamepad &&
                        sdl.gamepad == nullptr) {
                        show_error(sdl.window,
                                   "Connect a gamepad before configuring it.");
                    } else {
                        begin_binding_configuration(
                            bindings, configuration_backup, configuring,
                            action == ControlsAction::keyboard
                                ? BindingDevice::keyboard
                                : BindingDevice::gamepad);
                    }
                }
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_G &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0 && emulator) {
#ifndef __ANDROID__
                release_all_buttons(*emulator);
                cheat_manager.open(sdl.window);
#endif
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_P &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                choose_display_palette(emulator.get(), sdl, preference_path,
                                       display_palette);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key >= SDLK_1 && event.key.key <= SDLK_9 &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0) {
                const auto index = static_cast<std::size_t>(event.key.key - SDLK_1);
                if (index < recent.size()) pending_rom = recent[index];
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_R &&
                       (event.key.mod & SDL_KMOD_CTRL) != 0 && emulator &&
                       !input_movie_active) {
                reset_requested = true;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_SPACE && emulator) {
                paused = !paused;
#ifndef __ANDROID__
                input_movie.release_all(*emulator);
#else
                release_all_buttons(*emulator);
#endif
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_F12 && emulator) {
#ifndef __ANDROID__
                release_all_buttons(*emulator);
                debugger.toggle(sdl.window);
#endif
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_F11) {
                fullscreen = !fullscreen;
                if (!SDL_SetWindowFullscreen(sdl.window, fullscreen)) {
                    fullscreen = !fullscreen;
                    show_error(sdl.window, SDL_GetError());
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                       event.key.key == SDLK_F1) {
                if (emulator) release_all_buttons(*emulator);
                show_help(sdl.window, bindings);
            } else if (event.key.key == SDLK_ESCAPE &&
                       event.type == SDL_EVENT_KEY_DOWN) {
                if (confirm_exit(sdl.window)) running = false;
            } else if (emulator && !event.key.repeat) {
                if (const auto button = keyboard_button(bindings, event.key.key)) {
#ifndef __ANDROID__
                    input_movie.set_button(*emulator, *button,
                                           event.type == SDL_EVENT_KEY_DOWN);
#else
                    emulator->set_button(*button,
                                         event.type == SDL_EVENT_KEY_DOWN);
#endif
                }
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                auto x = event.button.x;
                auto y = event.button.y;
                static_cast<void>(SDL_RenderCoordinatesFromWindow(
                    sdl.renderer, x, y, &x, &y));
                if (dashboard_visible) {
                    const auto item_count =
                        dashboard_items(emulator != nullptr, recent).size();
                    if (const auto selected = dashboard_row_at(
                            x, y, dashboard_selection, item_count)) {
                        dashboard_selection = *selected;
                        activate_dashboard_selection(
                            dashboard_selection, recent, bindings, emulator.get(), dialog,
                            sdl, preference_path, pending_rom,
                            dashboard_visible, display_palette, running);
                    }
                }
#ifndef _WIN32
                else if (x < 20.0F && y < 17.0F) {
                    if (emulator) release_all_buttons(*emulator);
#ifdef __ANDROID__
                    open_android_library();
#else
#ifdef _WIN32
                    SDL_HideWindow(sdl.window);
#endif
                    dashboard_visible = true;
                    dashboard_selection = 0;
#endif
                }
#endif
            }
            break;
#ifdef __ANDROID__
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP: {
            const auto finger = event.tfinger.fingerID;
            const auto [touch_x, touch_y] = window_touch_position(event.tfinger);
            if (dashboard_visible) {
                if (event.type == SDL_EVENT_FINGER_UP) {
                    const auto [logical_x, logical_y] =
                        logical_touch_position(event.tfinger, sdl);
                    const auto item_count =
                        dashboard_items(emulator != nullptr, recent).size();
                    if (const auto selected = dashboard_row_at(
                            logical_x * gameboy::Ppu::screen_width,
                            logical_y * gameboy::Ppu::screen_height,
                            dashboard_selection, item_count)) {
                        dashboard_selection = *selected;
                        activate_dashboard_selection(
                            dashboard_selection, recent, bindings, emulator.get(), dialog,
                            sdl, preference_path, pending_rom,
                            dashboard_visible, display_palette, running);
                    }
                }
                break;
            }
            const auto existing = std::find_if(
                sdl.touches.begin(), sdl.touches.end(),
                [finger](const SdlResources::TouchPoint& point) {
                    return point.id == finger;
                });
            if (event.type == SDL_EVENT_FINGER_UP) {
                if (existing != sdl.touches.end()) sdl.touches.erase(existing);
            } else if (existing == sdl.touches.end()) {
                sdl.touches.push_back({finger, touch_x, touch_y});
            } else {
                existing->x = touch_x;
                existing->y = touch_y;
            }
            if (event.type == SDL_EVENT_FINGER_DOWN &&
                touch_x < 0.13F && touch_y < 0.16F) {
                clear_touch_buttons(emulator.get(), sdl);
                open_android_library();
            }
            refresh_touch_buttons(emulator.get(), sdl);
            break;
        }
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            clear_touch_buttons(emulator.get(), sdl);
            flush_battery_safely(emulator.get());
            paused = true;
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            display_palette = load_display_palette(preference_path);
#ifdef __ANDROID__
            {
                const auto touch_settings =
                    load_touch_control_settings(preference_path);
                sdl.touch_settings = touch_settings;
            }
#endif
            if (emulator != nullptr) {
                emulator->set_dmg_compatibility_colors(
                    gameboy::display_palettes[display_palette]
                        .cgb_compatibility);
            }
            paused = false;
            break;
#endif
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (emulator) {
#ifndef __ANDROID__
                input_movie.release_all(*emulator);
#else
                release_all_buttons(*emulator);
#endif
            }
            fast_forward = false;
            rewind = false;
#ifdef __ANDROID__
            clear_touch_buttons(emulator.get(), sdl);
#endif
            stop_rumble(sdl);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (sdl.gamepad == nullptr) {
                sdl.gamepad = SDL_OpenGamepad(event.gdevice.which);
                sdl.rumble_warning_shown = false;
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (sdl.gamepad != nullptr &&
                SDL_GetGamepadID(sdl.gamepad) == event.gdevice.which) {
                SDL_CloseGamepad(sdl.gamepad);
                sdl.gamepad = nullptr;
                sdl.rumble_output_active = false;
                sdl.rumble_warning_shown = false;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (dashboard_visible) {
                if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) break;
                const auto item_count =
                    dashboard_items(emulator != nullptr, recent).size();
                dashboard_selection =
                    std::min(dashboard_selection, item_count - 1);
                const auto button = static_cast<SDL_GamepadButton>(
                    event.gbutton.button);
                if (button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
                    dashboard_selection = dashboard_selection == 0
                                              ? item_count - 1
                                              : dashboard_selection - 1;
                } else if (button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
                    dashboard_selection =
                        (dashboard_selection + 1) % item_count;
                } else if (button == SDL_GAMEPAD_BUTTON_SOUTH) {
                    activate_dashboard_selection(
                        dashboard_selection, recent, bindings, emulator.get(), dialog,
                        sdl, preference_path, pending_rom, dashboard_visible,
                        display_palette, running);
                } else if (button == SDL_GAMEPAD_BUTTON_EAST && emulator) {
                    dashboard_visible = false;
                }
            } else if (configuring &&
                configuring->device == BindingDevice::gamepad) {
                if (event.type != SDL_EVENT_GAMEPAD_BUTTON_DOWN) break;
                const auto pressed = static_cast<SDL_GamepadButton>(
                    event.gbutton.button);
                const auto duplicate = std::find(
                    bindings.gamepad_buttons.begin(),
                    bindings.gamepad_buttons.end(), pressed);
                if (duplicate != bindings.gamepad_buttons.end() &&
                    static_cast<std::size_t>(
                        duplicate - bindings.gamepad_buttons.begin()) !=
                        configuring->index) {
                    show_error(sdl.window,
                               "That gamepad button is already assigned.");
                    break;
                }
                bindings.gamepad_buttons[configuring->index] = pressed;
                ++configuring->index;
                if (configuring->index == bindings.gamepad_buttons.size()) {
                    configuring.reset();
                    save_bindings(preference_path, bindings);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Gamepad controls",
                        "Gamepad bindings saved.", sdl.window));
                }
                update_window_title(sdl.window, current_rom, paused,
                                    configuring);
            } else if (emulator) {
                if (const auto button = gamepad_button(bindings,
                                                       event.gbutton.button)) {
#ifndef __ANDROID__
                    input_movie.set_button(
                        *emulator, *button,
                        event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
#else
                    emulator->set_button(
                        *button, event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
#endif
                }
            }
            break;
        default:
            break;
        }
    }
}

void show_error(SDL_Window* window, const std::string& message) {
    std::cerr << "Error: " << message << '\n';
    static_cast<void>(SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR, "Go Bigger Boy (GBB)", message.c_str(), window));
}

#ifndef __ANDROID__
bool offer_update(const gbb_desktop::UpdateInfo& update,
                  gameboy::Emulator* emulator, SdlResources& sdl) {
    stop_rumble(sdl);
    if (emulator != nullptr) release_all_buttons(*emulator);
    const auto message =
        std::string("Go Bigger Boy ") + update.version +
        " is available.\n\nYou are running version " GBB_VERSION
        ". Would you like GBB to install the update and restart?\n\n"
        "You can keep playing while the verified archive downloads.";
    constexpr std::array<SDL_MessageBoxButtonData, 2> buttons{{
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Update now"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Later"},
    }};
    const SDL_MessageBoxData box{
        SDL_MESSAGEBOX_INFORMATION,
        sdl.window,
        "Go Bigger Boy update available",
        message.c_str(),
        static_cast<int>(buttons.size()),
        buttons.data(),
        nullptr,
    };
    auto selection = 0;
    return SDL_ShowMessageBox(&box, &selection) && selection == 1;
}

std::pair<std::filesystem::path, std::filesystem::path> installation_paths() {
    const auto* base = SDL_GetBasePath();
    if (base == nullptr) throw std::runtime_error(SDL_GetError());
    auto executable_directory = std::filesystem::u8path(base).lexically_normal();
#ifdef _WIN32
    const auto executable = executable_directory / "gbb.exe";
    return {executable_directory, executable};
#elif defined(__APPLE__)
    const auto bundle = executable_directory.parent_path().parent_path();
    const auto executable = bundle / "Contents" / "MacOS" /
                            "Go Bigger Boy";
    return {bundle.parent_path(), executable};
#else
    const auto executable = executable_directory / "gbb";
    const auto root = executable_directory.filename() == "bin"
                          ? executable_directory.parent_path()
                          : executable_directory;
    return {root, executable};
#endif
}

bool installation_is_writable(const std::filesystem::path& root) {
    const auto probe = root / ".gbb-update-write-test";
    std::ofstream output(probe, std::ios::trunc);
    if (!output) return false;
    output << "write test";
    output.close();
    std::error_code ignored;
    std::filesystem::remove(probe, ignored);
    return !ignored;
}
#endif

void close_camera(SdlResources& sdl) noexcept {
    if (sdl.camera != nullptr) {
        SDL_CloseCamera(sdl.camera);
        sdl.camera = nullptr;
    }
    if (sdl.camera_frame != nullptr) {
        SDL_DestroySurface(sdl.camera_frame);
        sdl.camera_frame = nullptr;
    }
    sdl.next_camera_frame = {};
    sdl.camera_warning_shown = false;
}

#ifdef __ANDROID__
void open_android_library() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return;
    const auto activity_class = environment->GetObjectClass(activity);
    if (activity_class != nullptr) {
        const auto method = environment->GetMethodID(
            activity_class, "openLibrary", "()V");
        if (method != nullptr) environment->CallVoidMethod(activity, method);
        environment->DeleteLocalRef(activity_class);
    }
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    environment->DeleteLocalRef(activity);
}

void leave_android_game(
    std::unique_ptr<gameboy::Emulator>& emulator, SdlResources& sdl,
    bool& dashboard_visible, bool& paused, bool& fast_forward, bool& rewind,
    RewindHistory& rewind_history, bool& running) {
    if (emulator == nullptr) {
        if (confirm_exit(sdl.window)) running = false;
        return;
    }
    clear_touch_buttons(emulator.get(), sdl);
    flush_battery_safely(emulator.get());
    if (!confirm_exit(sdl.window)) return;

    // Keep SDL's native thread alive while the Android library is shown.
    // SDLActivity cannot start a second native main loop after the first one
    // returns, so ending it here makes the next ROM launch freeze.
    release_all_buttons(*emulator);
    stop_rumble(sdl);
    close_camera(sdl);
    emulator.reset();
    rewind_history.clear();
    fast_forward = false;
    rewind = false;
    // Android uses LibraryActivity for its dashboard. Keep the SDL surface
    // blank while it is in the background instead of showing the legacy
    // pixel-art menu.
    dashboard_visible = false;
    paused = true;
    open_android_library();
}

std::optional<int> android_camera_orientation_correction_degrees() noexcept {
    auto* environment = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) return std::nullopt;

    static jmethodID orientation_method{};
    if (orientation_method == nullptr) {
        const auto activity_class = environment->GetObjectClass(activity);
        if (activity_class != nullptr) {
            orientation_method = environment->GetMethodID(
                activity_class, "getCameraOrientationCorrectionDegrees", "()I");
            environment->DeleteLocalRef(activity_class);
        }
    }
    std::optional<int> orientation;
    if (orientation_method != nullptr) {
        orientation = static_cast<int>(
            environment->CallIntMethod(activity, orientation_method));
    }
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        orientation.reset();
    }
    environment->DeleteLocalRef(activity);
    return orientation;
}
#endif

void configure_camera(SdlResources& sdl, const gameboy::Emulator& emulator) {
    close_camera(sdl);
    if (!emulator.has_camera()) return;

    if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
        std::cerr << "Warning: camera subsystem is unavailable: "
                  << SDL_GetError() << '\n';
        sdl.camera_warning_shown = true;
        return;
    }
    int count = 0;
    SDL_CameraID* cameras = SDL_GetCameras(&count);
    if (cameras == nullptr || count == 0) {
        SDL_free(cameras);
        std::cerr << "Warning: no webcam was found; the Game Boy Camera "
                     "will use its fallback image.\n";
        sdl.camera_warning_shown = true;
        return;
    }

    const auto camera_id = cameras[0];
    const auto camera_position = SDL_GetCameraPosition(camera_id);
    sdl.mirror_camera =
        camera_position == SDL_CAMERA_POSITION_FRONT_FACING;
    sdl.camera_back_facing =
        camera_position == SDL_CAMERA_POSITION_BACK_FACING;
    constexpr SDL_CameraSpec camera_spec{
        SDL_PIXELFORMAT_RGBA32,
        SDL_COLORSPACE_SRGB,
        static_cast<int>(gameboy::Cartridge::camera_width),
        static_cast<int>(gameboy::Cartridge::camera_height),
        15,
        1,
    };
    sdl.camera = SDL_OpenCamera(camera_id, &camera_spec);
    SDL_free(cameras);
    if (sdl.camera == nullptr) {
        std::cerr << "Warning: webcam could not be opened: " << SDL_GetError()
                  << '\n';
        sdl.camera_warning_shown = true;
        return;
    }
    sdl.camera_frame = SDL_CreateSurface(
        static_cast<int>(gameboy::Cartridge::camera_width),
        static_cast<int>(gameboy::Cartridge::camera_height),
        SDL_PIXELFORMAT_RGBA32);
    if (sdl.camera_frame == nullptr) {
        std::cerr << "Warning: webcam conversion surface could not be created: "
                  << SDL_GetError() << '\n';
        close_camera(sdl);
        sdl.camera_warning_shown = true;
    }
}

void update_camera_frame(gameboy::Emulator* emulator, SdlResources& sdl) {
    if (emulator == nullptr || !emulator->has_camera() || sdl.camera == nullptr) {
        return;
    }
    const auto permission = SDL_GetCameraPermissionState(sdl.camera);
    if (permission == SDL_CAMERA_PERMISSION_STATE_DENIED) {
        if (!sdl.camera_warning_shown) {
            std::cerr << "Warning: webcam permission was denied; the Game Boy "
                         "Camera will use its fallback image.\n";
            sdl.camera_warning_shown = true;
        }
        return;
    }
    if (permission != SDL_CAMERA_PERMISSION_STATE_APPROVED) return;

    const auto now = std::chrono::steady_clock::now();
    if (now < sdl.next_camera_frame) return;
    sdl.next_camera_frame = now + std::chrono::milliseconds(66);

    SDL_Surface* source = SDL_AcquireCameraFrame(sdl.camera, nullptr);
    if (source == nullptr) return;
    auto rotation_degrees = SDL_GetFloatProperty(
        SDL_GetSurfaceProperties(source), SDL_PROP_SURFACE_ROTATION_FLOAT, 0.0F);
#ifdef __ANDROID__
    if (const auto correction =
            android_camera_orientation_correction_degrees()) {
        rotation_degrees += static_cast<float>(
            sdl.camera_back_facing ? -*correction : *correction);
    }
#endif
    auto rotation_quarters = static_cast<int>(
        rotation_degrees / 90.0F + (rotation_degrees < 0.0F ? -0.5F : 0.5F));
    rotation_quarters = (rotation_quarters % 4 + 4) % 4;
    constexpr auto target_width = gameboy::Cartridge::camera_width;
    constexpr auto target_height = gameboy::Cartridge::camera_height;
    SDL_Surface* frame = source;
    if (source->format != SDL_PIXELFORMAT_RGBA32 ||
        source->w != static_cast<int>(target_width) ||
        source->h != static_cast<int>(target_height)) {
        SDL_Rect crop{0, 0, source->w, source->h};
        if (static_cast<std::int64_t>(source->w) * target_height >
            static_cast<std::int64_t>(source->h) * target_width) {
            crop.w = static_cast<int>(static_cast<std::int64_t>(source->h) *
                                      target_width / target_height);
            crop.x = (source->w - crop.w) / 2;
        } else {
            crop.h = static_cast<int>(static_cast<std::int64_t>(source->w) *
                                      target_height / target_width);
            crop.y = (source->h - crop.h) / 2;
        }
        if (!SDL_BlitSurfaceScaled(source, &crop, sdl.camera_frame, nullptr,
                                   SDL_SCALEMODE_NEAREST)) {
            SDL_ReleaseCameraFrame(sdl.camera, source);
            if (!sdl.camera_warning_shown) {
                std::cerr << "Warning: webcam frame conversion failed: "
                          << SDL_GetError() << '\n';
                sdl.camera_warning_shown = true;
            }
            return;
        }
        frame = sdl.camera_frame;
    }

    const auto needs_lock = SDL_MUSTLOCK(frame);
    if (needs_lock && !SDL_LockSurface(frame)) {
        SDL_ReleaseCameraFrame(sdl.camera, source);
        return;
    }
    std::array<std::uint8_t, target_width * target_height> grayscale{};
    const auto* pixels = static_cast<const std::uint8_t*>(frame->pixels);
    const auto rotated_width = rotation_quarters % 2 == 0 ? frame->w : frame->h;
    const auto rotated_height = rotation_quarters % 2 == 0 ? frame->h : frame->w;
    auto crop_x = 0;
    auto crop_y = 0;
    auto crop_width = rotated_width;
    auto crop_height = rotated_height;
    if (static_cast<std::int64_t>(rotated_width) * target_height >
        static_cast<std::int64_t>(rotated_height) * target_width) {
        crop_width = static_cast<int>(
            static_cast<std::int64_t>(rotated_height) * target_width /
            target_height);
        crop_x = (rotated_width - crop_width) / 2;
    } else {
        crop_height = static_cast<int>(
            static_cast<std::int64_t>(rotated_width) * target_height /
            target_width);
        crop_y = (rotated_height - crop_height) / 2;
    }
    for (std::size_t y = 0; y < target_height; ++y) {
        for (std::size_t x = 0; x < target_width; ++x) {
            auto rotated_x = crop_x + static_cast<int>(
                (x * 2 + 1) * static_cast<std::size_t>(crop_width) /
                (target_width * 2));
            const auto rotated_y = crop_y + static_cast<int>(
                (y * 2 + 1) * static_cast<std::size_t>(crop_height) /
                (target_height * 2));
            if (sdl.mirror_camera) {
                rotated_x = rotated_width - 1 - rotated_x;
            }
            auto sample_x = rotated_x;
            auto sample_y = rotated_y;
            switch (rotation_quarters) {
            case 1:
                sample_x = rotated_y;
                sample_y = frame->h - 1 - rotated_x;
                break;
            case 2:
                sample_x = frame->w - 1 - rotated_x;
                sample_y = frame->h - 1 - rotated_y;
                break;
            case 3:
                sample_x = frame->w - 1 - rotated_y;
                sample_y = rotated_x;
                break;
            default: break;
            }
            const auto* pixel = pixels + sample_y * frame->pitch + sample_x * 4;
            grayscale[y * target_width + x] = static_cast<std::uint8_t>(
                (77U * pixel[0] + 150U * pixel[1] + 29U * pixel[2]) >> 8);
        }
    }
    if (needs_lock) SDL_UnlockSurface(frame);
    SDL_ReleaseCameraFrame(sdl.camera, source);
    emulator->set_camera_frame(grayscale.data(), grayscale.size());
}

#ifdef __ANDROID__
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

    auto display_name = preferred_display_name.empty()
                            ? source
                            : preferred_display_name;
    if (const auto query = display_name.find_first_of("?#");
        query != std::string::npos) {
        display_name.resize(query);
    }
    // Android document URIs percent-encode the user-facing filename. Retain
    // that name so metadata and Libretro artwork matching survive the import.
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
        separator != std::string::npos) {
        display_name.erase(0, separator + 1);
    }
    for (auto& character : display_name) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 32 || std::string_view{"/\\:*?\"<>|"}.find(character) !=
                             std::string_view::npos) {
            character = '_';
        }
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
#endif

void load_rom(const std::string& path,
              std::unique_ptr<gameboy::Emulator>& emulator,
              const gameboy::DisplayPalette& palette, SdlResources& sdl,
              const std::filesystem::path& preference_path) {
#ifdef __ANDROID__
    std::size_t byte_count{};
    void* loaded = SDL_LoadFile(path.c_str(), &byte_count);
    if (loaded == nullptr) {
        throw std::runtime_error(std::string{"Could not read ROM: "} +
                                 SDL_GetError());
    }
    const std::unique_ptr<void, decltype(&SDL_free)> owned(loaded, SDL_free);
    const auto* begin = static_cast<const std::uint8_t*>(loaded);
    std::vector<std::uint8_t> bytes(begin, begin + byte_count);
    gameboy::Cartridge cartridge(std::move(bytes));
    if (cartridge.has_battery() && !preference_path.empty()) {
        const auto save_directory = preference_path / "saves";
        std::filesystem::create_directories(save_directory);
        std::ostringstream name;
        name << std::hex << std::setw(16) << std::setfill('0')
             << cartridge.rom_fingerprint() << ".gb";
        cartridge.set_persistence_path(save_directory / name.str());
    }
    auto replacement = std::make_unique<gameboy::Emulator>(
        std::move(cartridge));
#else
    static_cast<void>(preference_path);
    auto replacement = std::make_unique<gameboy::Emulator>(
        gameboy::Cartridge::from_file(std::filesystem::u8path(path)));
#endif
    replacement->bus().connect_printer();
    replacement->set_dmg_compatibility_colors(palette.cgb_compatibility);
    stop_rumble(sdl);
    flush_battery_safely(emulator.get());
    emulator = std::move(replacement);
    configure_camera(sdl, *emulator);
}

void present_menu_button(SdlResources& sdl) {
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_BLEND));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 220, 235, 220, 100));
    const SDL_FRect button{3, 3, 15, 11};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &button));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 150));
    const std::array<SDL_FRect, 3> menu_lines{{
        {6, 5, 9, 1.5F}, {6, 8, 9, 1.5F}, {6, 11, 9, 1.5F}}};
    for (const auto& line : menu_lines) {
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &line));
    }
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_NONE));
}

#ifdef __ANDROID__
void present_touch_controls(SdlResources& sdl) {
    const auto scale = std::clamp(sdl.touch_settings.scale,
                                  minimum_touch_scale, maximum_touch_scale);
    const auto opacity = std::clamp(sdl.touch_settings.opacity,
                                    minimum_touch_opacity,
                                    maximum_touch_opacity);
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_BLEND));
    int window_width = 1;
    int window_height = 1;
    static_cast<void>(SDL_GetWindowSize(sdl.window, &window_width,
                                        &window_height));
    const auto game_scale = touch_game_scale(sdl);
    const auto size = game_scale * scale;
    if (!SDL_SetRenderLogicalPresentation(
            sdl.renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)) {
        sdl_error("Could not prepare touch controls");
    }
    const auto draw = [&sdl, opacity](const SDL_FRect& rect,
                                      const bool pressed) {
        const auto alpha = static_cast<std::uint8_t>(std::clamp(
            opacity * 255.0F + (pressed ? 35.0F : 0.0F), 0.0F,
            255.0F));
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, pressed ? 139 : 220, pressed ? 207 : 235,
            pressed ? 105 : 220, alpha));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &rect));
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, pressed ? 236 : 248, pressed ? 224 : 252,
            pressed ? 148 : 242, alpha));
        static_cast<void>(SDL_RenderRect(sdl.renderer, &rect));
    };

    const auto point_for = [&sdl, window_width, window_height](
                               const std::size_t control) {
        const auto [x, y] = touch_control_position(sdl, control);
        return SDL_FPoint{x * static_cast<float>(window_width),
                          y * static_cast<float>(window_height)};
    };
    const auto dpad = point_for(0);
    const auto dpad_arm = 21.0F * size;
    const auto dpad_thickness = 14.0F * size;
    draw(SDL_FRect{dpad.x, dpad.y - dpad_thickness * 0.5F, dpad_arm,
                   dpad_thickness}, sdl.touch_buttons[0]);
    draw(SDL_FRect{dpad.x - dpad_arm, dpad.y - dpad_thickness * 0.5F,
                   dpad_arm, dpad_thickness}, sdl.touch_buttons[1]);
    draw(SDL_FRect{dpad.x - dpad_thickness * 0.5F, dpad.y - dpad_arm,
                   dpad_thickness, dpad_arm}, sdl.touch_buttons[2]);
    draw(SDL_FRect{dpad.x - dpad_thickness * 0.5F, dpad.y, dpad_thickness,
                   dpad_arm}, sdl.touch_buttons[3]);
    const std::array<float, 4> widths{{24.0F, 24.0F, 22.0F, 22.0F}};
    const std::array<float, 4> heights{{24.0F, 24.0F, 10.0F, 10.0F}};
    for (std::size_t control = 1; control < touch_control_count; ++control) {
        const auto point = point_for(control);
        const auto size_index = control - 1;
        const auto rect_width = widths[size_index] * size;
        const auto rect_height = heights[size_index] * size;
        draw(SDL_FRect{point.x - rect_width * 0.5F,
                       point.y - rect_height * 0.5F, rect_width, rect_height},
             sdl.touch_buttons[control + 3]);
    }
    if (!configure_video_pipeline(sdl, sdl.video_mode)) {
        sdl_error("Could not restore game presentation");
    }
    static_cast<void>(SDL_SetRenderDrawBlendMode(sdl.renderer,
                                                 SDL_BLENDMODE_NONE));
}
#endif

SDL_FColor voxel_color(const std::uint32_t pixel, const float shade) {
    const auto component = [pixel, shade](const unsigned shift) {
        const auto value = static_cast<float>((pixel >> shift) & 0xFFU) / 255.0F;
        return std::clamp(value * shade, 0.0F, 1.0F);
    };
    return {component(16), component(8), component(0), 1.0F};
}

void render_voxel_diorama(const gameboy::Emulator& emulator,
                          SdlResources& sdl,
                          const gameboy::DisplayPalette& palette) {
    // SDL_RenderGeometry is backed by the active SDL GPU renderer (D3D,
    // OpenGL, Metal or Vulkan).  We submit a real perspective mesh here and
    // keep the native framebuffer as a textured facade on top of it.  This
    // gives every platform the same deterministic voxel projection without
    // requiring platform-specific shader binaries.
    gbb::populate_gameboy_scene_snapshot(emulator, sdl.scene_snapshot);
    const auto& scene = sdl.scene_snapshot;
    const auto fingerprint = emulator.rom_fingerprint();
    if (!sdl.voxel_profile_loaded || sdl.voxel_profile_fingerprint != fingerprint) {
        sdl.voxel_profile = gbb::load_voxel_profile(sdl.voxel_profile_path, fingerprint);
        sdl.voxel_profile_fingerprint = fingerprint;
        sdl.voxel_profile_loaded = true;
    }
    const auto profile = sdl.voxel_profile;
    const auto& pixels = emulator.framebuffer();
    const auto native_colors = emulator.bus().cgb_mode() || palette.cgb_compatibility;
    gameboy::Ppu::Framebuffer colored_pixels{};
    const auto color_at = [&](const std::size_t index) {
        return native_colors
                   ? pixels[index]
                   : gameboy::apply_display_palette(pixels[index], palette);
    };
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto x = index % gameboy::Ppu::screen_width;
        const auto y = index / gameboy::Ppu::screen_width;
        auto pixel = color_at(index);
        if (sdl.video_mode == gameboy::VideoMode::lcd_shader) {
            pixel = gameboy::apply_lcd_shader(pixel, x, y);
        }
        colored_pixels[index] = pixel;
    }

    auto& vertices = sdl.voxel_vertices;
    auto& indices = sdl.voxel_indices;
    vertices.clear();
    indices.clear();
    vertices.reserve(12000);
    indices.reserve(18000);
    const auto radians = [](const float degrees) {
        return degrees * 0.01745329251994329577F;
    };
    const auto yaw = radians(profile.camera_yaw);
    const auto pitch = radians(profile.camera_pitch);
    const auto yaw_cos = std::cos(yaw);
    const auto yaw_sin = std::sin(yaw);
    const auto pitch_cos = std::cos(pitch);
    const auto pitch_sin = std::sin(pitch);
    const auto project = [&](const float x, const float y, const float z) {
        const auto centered_x = x - 80.0F;
        const auto centered_y = y - 72.0F;
        const auto rotated_x = centered_x * yaw_cos - centered_y * yaw_sin;
        const auto rotated_y = centered_x * yaw_sin + centered_y * yaw_cos;
        const auto pitched_y = rotated_y * pitch_cos - z * pitch_sin;
        const auto depth = rotated_y * pitch_sin + z * pitch_cos;
        const auto perspective = 1.0F /
            std::max(0.35F, 1.0F + depth * profile.perspective);
        return SDL_FPoint{80.0F + rotated_x * profile.zoom * perspective,
                          72.0F + pitched_y * profile.zoom * perspective};
    };
    const auto add_quad = [&](const SDL_FPoint a, const SDL_FPoint b,
                              const SDL_FPoint c, const SDL_FPoint d,
                              const SDL_FColor color) {
        const auto base = static_cast<int>(vertices.size());
        vertices.push_back({a, color, {0.0F, 0.0F}});
        vertices.push_back({b, color, {0.0F, 0.0F}});
        vertices.push_back({c, color, {0.0F, 0.0F}});
        vertices.push_back({d, color, {0.0F, 0.0F}});
        indices.insert(indices.end(), {base, base + 1, base + 2,
                                       base, base + 2, base + 3});
    };
    const auto bit_count = [](std::uint8_t value) {
        unsigned count = 0;
        while (value != 0) {
            value = static_cast<std::uint8_t>(value &
                                              static_cast<std::uint8_t>(value - 1));
            ++count;
        }
        return count;
    };
    const auto average_tile_color = [&](const unsigned tile_x,
                                        const unsigned tile_y) {
        unsigned red = 0;
        unsigned green = 0;
        unsigned blue = 0;
        for (unsigned y = 0; y < 8; ++y) {
            for (unsigned x = 0; x < 8; ++x) {
                const auto pixel = colored_pixels[(tile_y * 8 + y) * 160 +
                                                  tile_x * 8 + x];
                red += (pixel >> 16) & 0xFFU;
                green += (pixel >> 8) & 0xFFU;
                blue += pixel & 0xFFU;
            }
        }
        return UINT32_C(0xFF000000) |
               ((red / 64U) << 16) | ((green / 64U) << 8) | (blue / 64U);
    };
    struct VoxelColumn {
        float x{};
        float y{};
        float width{8.0F};
        float extent_y{8.0F};
        float height{};
        float sort_depth{};
        std::uint32_t color{};
        bool sprite{};
    };
    std::vector<VoxelColumn> columns;
    columns.reserve(20 * 18);
    for (unsigned tile_y = 0; tile_y < 18; ++tile_y) {
        for (unsigned tile_x = 0; tile_x < 20; ++tile_x) {
            const auto map_x = (static_cast<unsigned>(scene.scx) / 8U + tile_x) & 31U;
            const auto map_y = (static_cast<unsigned>(scene.scy) / 8U + tile_y) & 31U;
            const auto map_index = map_y * 32U + map_x;
            const auto tile = scene.background.tile_ids[map_index];
            const auto attributes = scene.background.attributes[map_index];
            const auto bank = scene.cgb_mode && (attributes & 0x08U) != 0 ? 1U : 0U;
            const auto tile_index = scene.background.tile_data_unsigned
                                        ? static_cast<unsigned>(tile)
                                        : tile < 0x80U
                                              ? static_cast<unsigned>(tile) + 256U
                                              : static_cast<unsigned>(tile);
            if (tile_index >= scene.tile_count ||
                bank >= scene.tile_banks) {
                continue;
            }
            const auto tile_offset = bank * scene.tile_bank_stride +
                                     tile_index * scene.tile_size_bytes;
            unsigned occupancy = 0;
            for (std::size_t row = 0; row < 8; ++row) {
                const auto low = scene.tile_data[tile_offset + row * 2];
                const auto high = scene.tile_data[tile_offset + row * 2 + 1];
                occupancy += bit_count(low) + bit_count(high);
            }
            if (occupancy == 0) continue;
            const auto depth = profile.depth_scale *
                               (1.0F + std::min(5.0F,
                                                static_cast<float>(occupancy) / 24.0F));
            const auto x = static_cast<float>(tile_x * 8);
            const auto y = static_cast<float>(tile_y * 8);
            const auto centered_y = y + 4.0F - 72.0F;
            const auto centered_x = x + 4.0F - 80.0F;
            const auto rotated_y = centered_x * yaw_sin + centered_y * yaw_cos;
            columns.push_back({x, y, 8.0F, 8.0F, depth,
                               rotated_y * pitch_sin + depth * pitch_cos,
                               average_tile_color(tile_x, tile_y), false});
        }
    }
    const auto sprite_height = (scene.lcdc & 0x04U) != 0 ? 16.0F : 8.0F;
    for (const auto& sprite : scene.sprites) {
        if (!sprite.visible) continue;
        const auto x = static_cast<float>(sprite.screen_x);
        const auto y = static_cast<float>(sprite.screen_y);
        const auto centered_y = y + sprite_height * 0.5F - 72.0F;
        const auto centered_x = x + 4.0F - 80.0F;
        const auto rotated_y = centered_x * yaw_sin + centered_y * yaw_cos;
        const auto sample_x = std::clamp<int>(sprite.screen_x, 0, 159);
        const auto sample_y = std::clamp<int>(sprite.screen_y, 0, 143);
        const auto color = colored_pixels[static_cast<std::size_t>(sample_y) * 160 +
                                          static_cast<std::size_t>(sample_x)];
        columns.push_back({x, y, 8.0F, sprite_height, profile.sprite_depth,
                           rotated_y * pitch_sin + profile.sprite_depth * pitch_cos,
                           color, true});
    }
    // SDL geometry has no portable depth buffer. Painter ordering gives us
    // deterministic opaque occlusion on software, OpenGL, D3D, Metal and
    // Vulkan renderers alike: farther columns are submitted first.
    std::stable_sort(columns.begin(), columns.end(),
                     [](const VoxelColumn& left, const VoxelColumn& right) {
                         return left.sort_depth > right.sort_depth;
                     });
    for (const auto& column : columns) {
            const auto x = column.x;
            const auto y = column.y;
            const auto depth = column.height;
            const auto width = column.width;
            const auto extent_y = column.extent_y;
            const auto base_a = project(x, y, 0.0F);
            const auto base_b = project(x + width, y, 0.0F);
            const auto base_c = project(x + width, y + extent_y, 0.0F);
            const auto base_d = project(x, y + extent_y, 0.0F);
            const auto top_a = project(x, y, depth);
            const auto top_b = project(x + width, y, depth);
            const auto top_c = project(x + width, y + extent_y, depth);
            const auto top_d = project(x, y + extent_y, depth);
            const auto tile_color = column.color;
            add_quad(base_a, base_b, top_b, top_a,
                     voxel_color(tile_color,
                                 (column.sprite ? 0.30F : 0.34F) * profile.lighting));
            add_quad(base_b, base_c, top_c, top_b,
                     voxel_color(tile_color,
                                 (column.sprite ? 0.42F : 0.46F) * profile.lighting));
            add_quad(base_c, base_d, top_d, top_c,
                     voxel_color(tile_color,
                                 (column.sprite ? 0.54F : 0.58F) * profile.lighting));
            add_quad(base_d, base_a, top_a, top_d,
                     voxel_color(tile_color,
                                 (column.sprite ? 0.36F : 0.40F) * profile.lighting));
            add_quad(top_a, top_b, top_c, top_d,
                     voxel_color(tile_color,
                                 (column.sprite ? 0.62F : 0.72F) * profile.lighting));
    }
    if (!indices.empty() && !SDL_RenderGeometry(
                                 sdl.renderer, nullptr, vertices.data(),
                                 static_cast<int>(vertices.size()), indices.data(),
                                 static_cast<int>(indices.size()))) {
        sdl_error("Could not render voxel diorama geometry");
    }
    if (profile.framebuffer_facade) {
        if (!SDL_UpdateTexture(sdl.texture, nullptr, colored_pixels.data(),
                               static_cast<int>(160 * sizeof(std::uint32_t))) ||
            !SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr)) {
            sdl_error("Could not present voxel diorama facade");
        }
    }
}

#if !defined(_WIN32) && !defined(__ANDROID__)
void present_dashboard(SdlResources& sdl,
                       const std::vector<std::string>& recent,
                       const bool can_resume, std::size_t& selection) {
    const auto items = dashboard_items(can_resume, recent);
    selection = std::min(selection, items.size() - 1);
    const auto first = dashboard_first_visible(selection, items.size());
    const auto visible = std::min(dashboard_visible_rows, items.size() - first);

    // Keep the SDL dashboard visually aligned with the native desktop
    // dashboard: a deep navy canvas, bright cyan accents, and high-contrast
    // cards.  The logical 160x144 layout is intentionally compact so it
    // scales cleanly on small windows and on the Android renderer too.
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 8, 12, 20, 255));
    const SDL_FRect canvas{0, 0, 160, 144};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &canvas));
    const SDL_FRect header{0, 0, 160, 34};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &header));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 69, 207, 238, 255));
    const SDL_FRect accent{9, 31, 142, 2};
    static_cast<void>(SDL_RenderFillRect(sdl.renderer, &accent));
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, 5,
                                          "GO BIGGER BOY"));
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 177, 192, 208, 255));
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, 18,
                                          "GAME LIBRARY"));

    for (std::size_t row = 0; row < visible; ++row) {
        const auto index = first + row;
        const auto selected = index == selection;
        const auto y = dashboard_first_row_y +
                       static_cast<float>(row) * dashboard_row_height;
        const SDL_FRect card{9, y, 142, 15};
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, selected ? 20 : 20, selected ? 77 : 29,
            selected ? 101 : 42, 255));
        static_cast<void>(SDL_RenderFillRect(sdl.renderer, &card));
        static_cast<void>(SDL_SetRenderDrawColor(
            sdl.renderer, selected ? 230 : 137, selected ? 249 : 160,
            selected ? 255 : 183, 255));
        static_cast<void>(SDL_RenderRect(sdl.renderer, &card));
        const auto label = std::string(selected ? "> " : "  ") +
                           dashboard_text(items[index].label, 14);
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, y + 3,
                                              label.c_str()));
    }

    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 137, 160, 183, 255));
    if (first > 0) {
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 153, 39, "^"));
    }
    if (first + visible < items.size()) {
        static_cast<void>(SDL_RenderDebugText(sdl.renderer, 153, 111, "v"));
    }
    static_cast<void>(SDL_RenderDebugText(sdl.renderer, 13, 134,
                                          "UP/DOWN  ENTER SELECT"));
}
#endif

void present(const gameboy::Emulator* emulator, SdlResources& sdl,
             const gameboy::DisplayPalette& palette,
             const std::vector<std::string>& recent,
             const bool dashboard_visible,
             std::size_t& dashboard_selection) {
    static_cast<void>(SDL_SetRenderDrawColor(sdl.renderer, 16, 20, 16, 255));
    if (!SDL_RenderClear(sdl.renderer)) {
        sdl_error("Could not clear framebuffer");
    }
    if (dashboard_visible) {
#if !defined(_WIN32) && !defined(__ANDROID__)
        present_dashboard(sdl, recent, emulator != nullptr,
                          dashboard_selection);
#else
        static_cast<void>(recent);
        static_cast<void>(dashboard_selection);
#endif
    } else if (emulator != nullptr) {
        if (sdl.video_mode == gameboy::VideoMode::voxel_diorama) {
            render_voxel_diorama(*emulator, sdl, palette);
        } else {
        const auto& pixels = emulator->framebuffer();
        const auto native_colors =
            emulator->bus().cgb_mode() || palette.cgb_compatibility;
        gameboy::Ppu::Framebuffer colored_pixels{};
        const auto color_at = [&](const std::size_t source_index) {
            return native_colors
                       ? pixels[source_index]
                       : gameboy::apply_display_palette(pixels[source_index],
                                                        palette);
        };
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            auto pixel = color_at(index);
            const auto x = index % gameboy::Ppu::screen_width;
            const auto y = index / gameboy::Ppu::screen_width;
            if (sdl.video_mode == gameboy::VideoMode::sharp_smoothing) {
                const auto left = x == 0 ? index : index - 1;
                const auto right = x + 1 == gameboy::Ppu::screen_width
                                       ? index : index + 1;
                const auto up = y == 0 ? index : index - gameboy::Ppu::screen_width;
                const auto down = y + 1 == gameboy::Ppu::screen_height
                                      ? index : index + gameboy::Ppu::screen_width;
                pixel = gameboy::apply_sharp_smoothing(
                    pixel, color_at(left), color_at(right), color_at(up),
                    color_at(down));
            } else if (sdl.video_mode == gameboy::VideoMode::lcd_shader) {
                pixel = gameboy::apply_lcd_shader(pixel, x, y);
            }
            colored_pixels[index] = pixel;
        }
        if (!SDL_UpdateTexture(sdl.texture, nullptr, colored_pixels.data(),
                               static_cast<int>(gameboy::Ppu::screen_width *
                                                sizeof(std::uint32_t))) ||
            !SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr)) {
            sdl_error("Could not present framebuffer");
        }
        }
    }
#ifdef __ANDROID__
    if (!dashboard_visible && emulator != nullptr) {
        present_touch_controls(sdl);
    }
#endif
#ifndef _WIN32
    if (!dashboard_visible && emulator != nullptr) present_menu_button(sdl);
#endif
    if (!SDL_RenderPresent(sdl.renderer)) {
        sdl_error("Could not present framebuffer");
    }
}

void submit_audio(gameboy::Emulator* emulator, SdlResources& sdl,
                  const bool fast_forward = false) {
    if (emulator == nullptr) return;
    auto samples = emulator->take_audio_samples();
    if (fast_forward && !samples.empty()) {
        samples = gbb::downsample_audio_box(samples, 2, fast_forward_factor);
    }
    if (sdl.audio_stream == nullptr || samples.empty()) return;
    constexpr auto maximum_queued_bytes =
        gbb::audio_queue_bytes(gameboy::Apu::sample_rate, 2, 200);
    if (SDL_GetAudioStreamQueued(sdl.audio_stream) >
        static_cast<int>(maximum_queued_bytes)) {
        // A debugger pause, window drag, or suspended mobile activity can leave
        // stale audio behind. Recover latency rather than playing it seconds
        // after the corresponding frame.
        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
    }
    if (!SDL_PutAudioStreamData(
            sdl.audio_stream, samples.data(),
            static_cast<int>(samples.size() * sizeof(samples.front())))) {
        sdl_error("Could not queue audio samples");
    }
}

} // namespace

#ifdef __ANDROID__
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeLibraryEntries(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory = environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto library = gameboy::RomLibrary::load(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);

    const auto string_class = environment->FindClass("java/lang/String");
    const auto& entries = library.entries();
    auto result = environment->NewObjectArray(
        static_cast<jsize>(entries.size()), string_class, nullptr);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        std::ostringstream encoded;
        encoded << std::hex << entry.metadata.fingerprint << '\x1f'
                << entry.metadata.crc32 << '\x1f'
                << entry.path.u8string() << '\x1f'
                << entry.metadata.title << '\x1f'
                << gameboy::platform_name(entry.metadata.platform) << '\x1f'
                << entry.metadata.language << '\x1f'
                << gameboy::cover_system_name(entry.metadata.platform) << '\x1f'
                << entry.metadata.cover_name << '\x1f'
                << std::dec << entry.last_played;
        const auto text = encoded.str();
        const auto value = environment->NewStringUTF(text.c_str());
        environment->SetObjectArrayElement(
            result, static_cast<jsize>(index), value);
        environment->DeleteLocalRef(value);
    }
    environment->DeleteLocalRef(string_class);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeRemoveLibraryEntry(
    JNIEnv* environment, jclass, jstring directory, jstring fingerprint) {
    const auto* raw_directory = environment->GetStringUTFChars(directory, nullptr);
    const auto* raw_fingerprint =
        environment->GetStringUTFChars(fingerprint, nullptr);
    if (raw_directory == nullptr || raw_fingerprint == nullptr) {
        if (raw_directory != nullptr) {
            environment->ReleaseStringUTFChars(directory, raw_directory);
        }
        if (raw_fingerprint != nullptr) {
            environment->ReleaseStringUTFChars(fingerprint, raw_fingerprint);
        }
        return JNI_FALSE;
    }
    auto library = gameboy::RomLibrary::load(
        std::filesystem::u8path(raw_directory));
    std::uint64_t value{};
    std::istringstream parser(raw_fingerprint);
    parser >> std::hex >> value;
    const auto removed = parser && library.remove(value);
    if (removed) library.save(std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    environment->ReleaseStringUTFChars(fingerprint, raw_fingerprint);
    return removed ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchControlScale(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return 1.35F;
    const auto settings = load_touch_control_settings(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return settings.scale;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchControlOpacity(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return 0.78F;
    const auto settings = load_touch_control_settings(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    return settings.opacity;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeTouchControlLayout(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto positions = load_touch_control_layout(
        std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    const auto result = environment->NewFloatArray(
        static_cast<jsize>(positions.size()));
    if (result != nullptr) {
        environment->SetFloatArrayRegion(
            result, 0, static_cast<jsize>(positions.size()), positions.data());
    }
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetTouchControlSettings(
    JNIEnv* environment, jclass, jstring directory, const jfloat scale,
    const jfloat opacity) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return;
    save_touch_control_settings(std::filesystem::u8path(raw_directory), scale,
                                opacity);
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetTouchControlLayout(
    JNIEnv* environment, jclass, jstring directory, jfloatArray values) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr || values == nullptr) {
        if (raw_directory != nullptr) {
            environment->ReleaseStringUTFChars(directory, raw_directory);
        }
        return;
    }
    const auto length = environment->GetArrayLength(values);
    if (length >= static_cast<jsize>(touch_layout_count * touch_layout_stride)) {
        std::array<float, touch_layout_count * touch_layout_stride> positions{};
        environment->GetFloatArrayRegion(
            values, 0, static_cast<jsize>(positions.size()), positions.data());
        save_touch_control_layout(std::filesystem::u8path(raw_directory),
                                  positions);
    }
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeResetTouchControlLayout(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return;
    const TouchControlSettings defaults;
    save_touch_control_layout(std::filesystem::u8path(raw_directory),
                              defaults.positions);
    environment->ReleaseStringUTFChars(directory, raw_directory);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeVideoMode(
    JNIEnv* environment, jclass, jstring directory) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    if (raw_directory == nullptr) return nullptr;
    const auto mode = load_video_mode(std::filesystem::u8path(raw_directory));
    environment->ReleaseStringUTFChars(directory, raw_directory);
    const auto id = gameboy::video_mode_info(mode).id;
    return environment->NewStringUTF(std::string{id}.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_LibraryActivity_nativeSetVideoMode(
    JNIEnv* environment, jclass, jstring directory, jstring mode) {
    const auto* raw_directory =
        environment->GetStringUTFChars(directory, nullptr);
    const auto* raw_mode = mode == nullptr
                               ? nullptr
                               : environment->GetStringUTFChars(mode, nullptr);
    if (raw_directory != nullptr && raw_mode != nullptr) {
        save_video_mode(std::filesystem::u8path(raw_directory),
                        gameboy::video_mode_from_id(raw_mode));
    }
    if (raw_mode != nullptr) environment->ReleaseStringUTFChars(mode, raw_mode);
    if (raw_directory != nullptr) {
        environment->ReleaseStringUTFChars(directory, raw_directory);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_GbbActivity_nativeAndroidBackPressed(
    JNIEnv*, jclass) {
    android_back_requested.store(true);
}

extern "C" JNIEXPORT void JNICALL
Java_com_danielseim_gbb_GbbActivity_nativeOpenRom(
    JNIEnv* environment, jclass, jstring rom, jstring display_name) {
    const auto* raw_rom = environment->GetStringUTFChars(rom, nullptr);
    if (raw_rom == nullptr) return;
    const auto* raw_name = display_name == nullptr
                               ? nullptr
                               : environment->GetStringUTFChars(display_name,
                                                                 nullptr);
    {
        std::lock_guard<std::mutex> lock(android_rom_request_mutex);
        android_rom_request = AndroidRomRequest{
            raw_rom, raw_name == nullptr ? std::string{} : std::string{raw_name}};
    }
    if (raw_name != nullptr) {
        environment->ReleaseStringUTFChars(display_name, raw_name);
    }
    environment->ReleaseStringUTFChars(rom, raw_rom);
}
#endif

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "Go Bigger Boy " GBB_VERSION << '\n';
            return EXIT_SUCCESS;
        }
        DialogState dialog;
#ifdef _WIN32
        const auto start_with_library = argc != 2;
#else
        constexpr auto start_with_library = false;
#endif
        SdlResources sdl(start_with_library);
#ifdef _WIN32
        DesktopMenuBar desktop_menu;
        desktop_menu.attach(sdl.window);
#endif
        const auto preference_path = preference_directory();
        sdl.voxel_profile_path = preference_path.empty()
                                    ? std::filesystem::path{}
                                    : preference_path / "voxel-profiles.ini";
        gbb::ensure_voxel_profile_file(sdl.voxel_profile_path);
        restore_game_window_geometry(sdl.window, preference_path);
        auto rom_library = load_rom_library(preference_path);
        auto recent_roms = recent_paths(rom_library);
        auto bindings = load_bindings(preference_path);
        auto configuration_backup = bindings;
        auto display_palette = load_display_palette(preference_path);
        auto video_mode = load_video_mode(preference_path);
        if (!configure_video_pipeline(sdl, video_mode)) {
            sdl_error("Could not configure video pipeline");
        }
#ifdef __ANDROID__
        const auto touch_settings = load_touch_control_settings(preference_path);
        sdl.touch_settings = touch_settings;
#endif
        std::unique_ptr<gameboy::Emulator> emulator;
        std::string current_rom;
        std::optional<std::string> pending_rom;
#ifdef __ANDROID__
        std::string pending_rom_name;
#endif
        std::optional<BindingConfiguration> configuring;
#ifndef __ANDROID__
        gbb_desktop::UpdateChecker update_checker{GBB_VERSION};
        std::optional<gbb_desktop::UpdateInfo> available_update;
        std::unique_ptr<gbb_desktop::UpdateDownload> update_download;
        bool update_check_complete = false;
#endif
#ifdef _WIN32
        bool reveal_sdl_after_present = false;
#endif
        auto paused = false;
        auto fullscreen = false;
        auto fast_forward = false;
        auto rewind = false;
        auto reset_requested = false;
        auto running = true;
#ifdef __ANDROID__
        // The native Android LibraryActivity owns the dashboard. The SDL
        // surface must never render the legacy pixel-art dashboard.
        auto dashboard_visible = false;
#else
        auto dashboard_visible = argc != 2;
#endif
        std::size_t dashboard_selection = 0;
        std::uint64_t print_sequence = 0;
        RewindHistory rewind_history;
#ifndef __ANDROID__
        DesktopDebugger debugger;
        InputMovie input_movie;
        TasEditor tas_editor;
        SpriteEditor sprite_editor;
        CheatManager cheat_manager;
        const auto movie_path = preference_path / "replays" /
                                "last-input.gbbmovie";
        const auto sprite_patch_path = preference_path / "sprite-patches" /
                                       "last-sprite-edit.gbbtiles";
        const auto sprite_ips_path = preference_path / "sprite-patches" /
                                     "last-sprite-edit.ips";
#endif

#ifdef __ANDROID__
        if (argc >= 2) {
            pending_rom = argv[1];
            if (argc >= 3) pending_rom_name = argv[2];
        } else {
#else
        if (argc == 2) {
            pending_rom = argv[1];
        } else {
            if (argc > 2) {
                show_error(sdl.window, "Only one ROM can be opened at a time.");
            }
#endif
            static_cast<void>(SDL_SetWindowTitle(
                sdl.window, "Go Bigger Boy (GBB) - Game Library"));
        }
#ifdef _WIN32
        if (dashboard_visible) SDL_HideWindow(sdl.window);
#endif

        using Clock = std::chrono::steady_clock;
        constexpr auto cycles_per_frame = 70224U;
        const auto frame_duration = std::chrono::duration<double>(
            static_cast<double>(cycles_per_frame) / 4194304.0);
        auto next_frame = Clock::now();

        while (running) {
#ifndef __ANDROID__
            if (!update_check_complete) {
                std::string update_error;
                std::optional<gbb_desktop::UpdateInfo> update_result;
                if (update_checker.take_result(update_result, update_error)) {
                    update_check_complete = true;
                    if (!update_error.empty()) {
                        std::cerr << "Warning: update check unavailable: "
                                  << update_error << '\n';
                    }
                    available_update = std::move(update_result);
                }
            }
#endif
#ifdef _WIN32
            if (dashboard_visible && update_check_complete &&
                !available_update && !update_download) {
                save_game_window_geometry(sdl.window, preference_path);
                SDL_HideWindow(sdl.window);
                gbb_desktop::KeyboardBindings dashboard_bindings{};
                for (std::size_t index = 0; index < bindings.keys.size();
                     ++index) {
                    for (std::size_t slot = 0;
                         slot < bindings.keys[index].size(); ++slot) {
                        dashboard_bindings[index][slot] =
                            static_cast<std::int64_t>(bindings.keys[index][slot]);
                    }
                }
                gbb_desktop::ActionBindings dashboard_actions{};
                for (std::size_t index = 0; index < dashboard_actions.size();
                     ++index) {
                    dashboard_actions[index] = static_cast<std::int64_t>(
                        bindings.shortcuts[index]);
                }
                const auto result = gbb_desktop::show_windows_dashboard(
                    nullptr, rom_library, emulator != nullptr,
                    emulator ? emulator->rom_fingerprint() : 0,
                    display_palette,
                    sdl.video_mode,
                    dashboard_bindings, dashboard_actions,
                    preference_path);
                if (!result.removed_fingerprints.empty()) {
                    for (const auto fingerprint : result.removed_fingerprints) {
                        static_cast<void>(rom_library.remove(fingerprint));
                    }
                    rom_library.save(preference_path);
                    recent_roms = recent_paths(rom_library);
                }
                if (result.palette_changed &&
                    result.palette < gameboy::display_palettes.size()) {
                    display_palette = result.palette;
                    save_display_palette(preference_path, display_palette);
                    if (emulator) {
                        emulator->set_dmg_compatibility_colors(
                            gameboy::display_palettes[display_palette]
                                .cgb_compatibility);
                    }
                }
                if (result.video_mode_changed) {
                    sdl.video_mode = result.video_mode;
                    save_video_mode(preference_path, sdl.video_mode);
                    if (!configure_video_pipeline(sdl, sdl.video_mode)) {
                        show_error(sdl.window,
                                   "Could not configure the selected video pipeline.");
                    }
                }
                if (result.voxel_profile_changed) {
                    sdl.voxel_profile_loaded = false;
                }
                if (result.keyboard_bindings_changed) {
                    for (std::size_t index = 0; index < bindings.keys.size();
                         ++index) {
                        for (std::size_t slot = 0;
                             slot < bindings.keys[index].size(); ++slot) {
                            bindings.keys[index][slot] =
                                static_cast<SDL_Keycode>(
                                    result.keyboard_bindings[index][slot]);
                        }
                    }
                    save_bindings(preference_path, bindings);
                }
                if (result.action_bindings_changed) {
                    for (std::size_t index = 0;
                         index < bindings.shortcuts.size(); ++index) {
                        bindings.shortcuts[index] = static_cast<SDL_Keycode>(
                            result.action_bindings[index]);
                    }
                    save_bindings(preference_path, bindings);
                }
                switch (result.action) {
                case gbb_desktop::DashboardResultAction::open_rom:
                    pending_rom = result.rom_path;
                    dashboard_visible = false;
                    break;
                case gbb_desktop::DashboardResultAction::resume:
                    dashboard_visible = false;
                    reveal_sdl_after_present = true;
                    break;
                case gbb_desktop::DashboardResultAction::quit:
                    running = false;
                    break;
                }
                if (!running) break;
            }
#endif
#ifdef __ANDROID__
            if (auto requested = take_android_rom_request()) {
                pending_rom = std::move(requested->path);
                pending_rom_name = std::move(requested->display_name);
            }
#endif
            process_events(emulator, sdl, dialog, preference_path, bindings,
                           configuration_backup, recent_roms, current_rom,
                           configuring, pending_rom, display_palette,
                           dashboard_visible, dashboard_selection, paused,
                           fullscreen, fast_forward, rewind, rewind_history,
                           reset_requested, running
#ifndef __ANDROID__
                           , debugger, input_movie, tas_editor, sprite_editor,
                           cheat_manager
#ifdef _WIN32
                           , desktop_menu
#endif
#endif
                           );

            std::optional<std::string> dialog_error;
            collect_dialog_result(dialog, pending_rom, dialog_error);
            if (dialog_error) show_error(sdl.window, *dialog_error);

            if (reset_requested) {
                if (!current_rom.empty()) pending_rom = current_rom;
                reset_requested = false;
            }

            if (pending_rom) {
                try {
#ifndef __ANDROID__
                    input_movie.stop(emulator.get());
                    tas_editor.close();
                    sprite_editor.reset_session();
                    cheat_manager.close();
#endif
                    auto requested_rom = *pending_rom;
#ifdef __ANDROID__
                    requested_rom =
                        persist_android_rom(requested_rom, preference_path,
                                            pending_rom_name);
#endif
                    const bool reopening_current =
                        emulator && requested_rom == current_rom;
                    load_rom(requested_rom, emulator,
                             gameboy::display_palettes[display_palette], sdl,
                             preference_path);
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                    current_rom = requested_rom;
                    if (!reopening_current) paused = false;
                    fast_forward = false;
                    rewind = false;
                    rewind_history.clear();
                    dashboard_visible = false;
#ifdef _WIN32
                    reveal_sdl_after_present = true;
#endif
                    rom_library.remember(
                        current_rom, gameboy::inspect_rom_file(current_rom));
#ifndef __ANDROID__
                    cheat_manager.load(preference_path,
                                       gameboy::inspect_rom_file(current_rom));
#endif
                    rom_library.save(preference_path);
                    recent_roms = recent_paths(rom_library);
                    update_window_title(sdl.window, current_rom, paused,
                                        configuring);
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                    if (!emulator) {
#ifdef _WIN32
                        SDL_HideWindow(sdl.window);
#endif
                        dashboard_visible = false;
                        dashboard_selection = 0;
#ifdef __ANDROID__
                        open_android_library();
#endif
                    }
                }
                pending_rom.reset();
#ifdef __ANDROID__
                pending_rom_name.clear();
#endif
                next_frame = Clock::now();
            }

#ifndef __ANDROID__
            if (emulator && debugger.take_record_toggle()) {
                try {
                    if (input_movie.recording()) {
                        input_movie.stop_and_save(movie_path, *emulator);
                    } else {
                        input_movie.stop(emulator.get());
                        input_movie.start_recording(*emulator);
                        rewind_history.clear();
                        paused = false;
                        fast_forward = false;
                        rewind = false;
                        debugger.run();
                    }
                } catch (const std::exception& error) {
                    input_movie.stop(emulator.get());
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator && debugger.take_replay_request()) {
                try {
                    input_movie.stop(emulator.get());
                    input_movie.start_replay(*emulator, movie_path);
                    rewind_history.clear();
                    paused = false;
                    fast_forward = false;
                    rewind = false;
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                    debugger.run();
                } catch (const std::exception& error) {
                    input_movie.stop(emulator.get());
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator && debugger.take_tas_request()) {
                input_movie.stop(emulator.get());
                tas_editor.open(sdl.window, *emulator);
                rewind_history.clear();
                debugger.pause();
            }
            if (emulator && debugger.take_sprite_request()) {
                input_movie.stop(emulator.get());
                sprite_editor.open(sdl.window, *emulator);
                rewind_history.clear();
                debugger.pause();
            }
            if (emulator && sprite_editor.take_save_patch_request()) {
                try {
                    sprite_editor.save_patch(*emulator, sprite_patch_path);
                    const auto message = "Sprite patch saved to:\n" +
                                         sprite_patch_path.string();
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "Sprite patch saved",
                        message.c_str(), sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator && sprite_editor.take_load_patch_request()) {
                try {
                    sprite_editor.load_patch(*emulator, sprite_patch_path);
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator && sprite_editor.take_export_ips_request()) {
                try {
                    const auto result = sprite_editor.export_ips(
                        *emulator, current_rom, sprite_ips_path);
                    const auto message =
                        "IPS patch saved to:\n" + sprite_ips_path.string() +
                        "\n\nTiles exported: " +
                        std::to_string(result.exported) +
                        "\nTiles skipped because their ROM source was "
                        "missing or ambiguous: " +
                        std::to_string(result.unresolved);
                    static_cast<void>(SDL_ShowSimpleMessageBox(
                        SDL_MESSAGEBOX_INFORMATION, "IPS patch exported",
                        message.c_str(), sdl.window));
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator && cheat_manager.take_fetch_request()) {
                try {
                    cheat_manager.fetch_archive();
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator && tas_editor.take_new_request()) {
                input_movie.stop(emulator.get());
                tas_editor.reset_from(*emulator);
                rewind_history.clear();
                debugger.pause();
            }
            if (emulator && tas_editor.take_save_request()) {
                try {
                    input_movie.stop(emulator.get());
                    input_movie.save_frame_inputs(
                        *emulator, movie_path, tas_editor.fingerprint(),
                        tas_editor.start_state(), tas_editor.frames());
                    static_cast<void>(emulator->take_audio_samples());
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                } catch (const std::exception& error) {
                    show_error(sdl.window, error.what());
                }
            }
            if (emulator && tas_editor.take_replay_request()) {
                try {
                    input_movie.stop(emulator.get());
                    input_movie.save_frame_inputs(
                        *emulator, movie_path, tas_editor.fingerprint(),
                        tas_editor.start_state(), tas_editor.frames());
                    input_movie.start_replay(*emulator, movie_path);
                    static_cast<void>(emulator->take_audio_samples());
                    rewind_history.clear();
                    paused = false;
                    fast_forward = false;
                    rewind = false;
                    if (sdl.audio_stream != nullptr) {
                        static_cast<void>(SDL_ClearAudioStream(sdl.audio_stream));
                    }
                    debugger.run();
                } catch (const std::exception& error) {
                    input_movie.stop(emulator.get());
                    show_error(sdl.window, error.what());
                }
            }
#endif

#ifndef __ANDROID__
            if (available_update && !dialog_active(dialog) && !configuring &&
                !pending_rom) {
                if (offer_update(*available_update, emulator.get(), sdl)) {
                    try {
                        const auto [root, executable] = installation_paths();
                        if (!installation_is_writable(root)) {
                            throw std::runtime_error(
                                "The installation directory is not writable. "
                                "Install GBB in a user-writable folder to use "
                                "automatic updates.");
                        }
                        const auto directory =
                            (preference_path.empty()
                                 ? std::filesystem::temp_directory_path() /
                                       "go-bigger-boy"
                                 : preference_path) /
                            "updates" / available_update->version;
                        update_download =
                            std::make_unique<gbb_desktop::UpdateDownload>(
                                *available_update, directory);
                        static_cast<void>(SDL_SetWindowTitle(
                            sdl.window,
                            "Go Bigger Boy (GBB) - Downloading update..."));
                    } catch (const std::exception& error) {
                        show_error(sdl.window, error.what());
                    }
                }
                available_update.reset();
                next_frame = Clock::now();
            }

            if (update_download) {
                std::optional<gbb_desktop::DownloadedUpdate> downloaded;
                std::string download_error;
                if (update_download->take_result(downloaded, download_error)) {
                    update_download.reset();
                    if (!download_error.empty() || !downloaded) {
                        show_error(sdl.window,
                                   download_error.empty()
                                       ? "The update download failed."
                                       : download_error);
                        update_window_title(sdl.window, current_rom, paused,
                                            configuring);
                    } else {
                        try {
                            const auto [root, executable] = installation_paths();
                            std::string installer_error;
                            if (!gbb_desktop::launch_update_installer(
                                    *downloaded, root, executable,
                                    installer_error)) {
                                throw std::runtime_error(installer_error);
                            }
                            running = false;
                        } catch (const std::exception& error) {
                            show_error(sdl.window, error.what());
                        }
                    }
                }
            }
#endif

            update_camera_frame(emulator.get(), sdl);
#ifndef __ANDROID__
            if (input_movie.replaying()) {
                fast_forward = false;
                rewind = false;
            }
#endif

            bool debugger_stepped = false;
            bool replay_ended = false;
            const auto step_emulator = [&]() {
#ifndef __ANDROID__
                if (input_movie.update_replay(*emulator)) {
                    replay_ended = true;
                    return cycles_per_frame;
                }
#endif
                return emulator->step();
            };
#ifndef __ANDROID__
            if (emulator && debugger.take_instruction_step()) {
                static_cast<void>(step_emulator());
                if (emulator->frame_ready()) emulator->consume_frame();
                rewind_history.clear();
                debugger_stepped = true;
            } else if (emulator && debugger.take_frame_step()) {
                if (emulator->frame_ready()) emulator->consume_frame();
                cheat_manager.apply(*emulator);
                unsigned cycles = 0;
                while (cycles < cycles_per_frame * 2U &&
                       !emulator->frame_ready()) {
                    cycles += step_emulator();
                }
                if (emulator->frame_ready()) emulator->consume_frame();
                rewind_history.clear();
                debugger_stepped = true;
            }
            const auto debugger_paused = debugger.execution_paused();
#else
            constexpr auto debugger_paused = false;
#endif
            if (emulator && !debugger_stepped && !paused && !debugger_paused &&
                !dashboard_visible && !configuring &&
                !dialog_active(dialog)) {
                if (rewind) {
                    if (!rewind_history.empty()) {
                        auto state = std::move(rewind_history.back());
                        rewind_history.pop_back();
                        emulator->load_state(state);
                        release_all_buttons(*emulator);
                        if (sdl.audio_stream != nullptr) {
                            static_cast<void>(SDL_ClearAudioStream(
                                sdl.audio_stream));
                        }
                    }
                } else {
                    const auto frames = fast_forward ? fast_forward_factor : 1U;
                    for (auto frame = 0U; frame < frames && running; ++frame) {
#ifndef __ANDROID__
                        cheat_manager.apply(*emulator);
#endif
                        rewind_history.push_back(emulator->save_state());
                        while (rewind_history.size() > maximum_rewind_frames) {
                            rewind_history.pop_front();
                        }
                        unsigned cycles = 0;
                        while (running && cycles < cycles_per_frame &&
                               !emulator->frame_ready()) {
                            cycles += step_emulator();
                        }
                        if (emulator->frame_ready()) emulator->consume_frame();
                    }
                }
            }
#ifndef __ANDROID__
            if (replay_ended) debugger.pause();
#endif
            update_rumble(emulator.get(), sdl,
                          !paused && !debugger_paused && !rewind &&
                              !dashboard_visible &&
                              !configuring &&
                              !dialog_active(dialog));
            submit_audio(emulator.get(), sdl, fast_forward);
            try {
                save_completed_prints(emulator.get(), sdl.window,
                                      preference_path, current_rom,
                                      print_sequence);
            } catch (const std::exception& error) {
                show_error(sdl.window, error.what());
            }
            present(emulator.get(), sdl,
                    gameboy::display_palettes[display_palette], recent_roms,
                    dashboard_visible, dashboard_selection);
#ifndef __ANDROID__
            if (emulator) {
                debugger.present(*emulator,
                                 gameboy::display_palettes[display_palette],
                                 input_movie);
            }
            tas_editor.present();
            sprite_editor.present(emulator.get());
            cheat_manager.present();
#endif
#ifdef _WIN32
            if (reveal_sdl_after_present && !dashboard_visible) {
                SDL_ShowWindow(sdl.window);
                reveal_sdl_after_present = false;
            }
#endif

            next_frame += std::chrono::duration_cast<Clock::duration>(
                frame_duration);
            const auto now = Clock::now();
            if (next_frame > now) {
                std::this_thread::sleep_until(next_frame);
            } else if (now - next_frame > std::chrono::milliseconds(100)) {
                next_frame = now;
            }
        }
#ifndef __ANDROID__
        if (emulator && input_movie.recording()) {
            try {
                input_movie.stop_and_save(movie_path, *emulator);
            } catch (const std::exception& error) {
                std::cerr << "Warning: could not save input recording: "
                          << error.what() << '\n';
            }
        }
#endif
        save_game_window_geometry(sdl.window, preference_path);
        flush_battery_safely(emulator.get());
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
