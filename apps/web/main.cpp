#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "gameboy/emulator.hpp"
#include "gameboy/display_palette.hpp"
#include "gameboy/video_pipeline.hpp"

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr auto cpu_frequency = 4194304.0;
constexpr auto cycles_per_frame = 70224.0;

constexpr std::array<gameboy::Button, 8> button_order{
    gameboy::Button::right, gameboy::Button::left, gameboy::Button::up,
    gameboy::Button::down, gameboy::Button::a, gameboy::Button::b,
    gameboy::Button::select, gameboy::Button::start,
};

struct WebApp {
    ~WebApp() {
        if (gamepad) SDL_CloseGamepad(gamepad);
        if (audio_stream) SDL_DestroyAudioStream(audio_stream);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
    }

    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Gamepad* gamepad{};
    SDL_AudioStream* audio_stream{};
    std::unique_ptr<gameboy::Emulator> emulator;
    gameboy::Ppu::Framebuffer display_pixels{};
    std::chrono::steady_clock::time_point previous_time{
        std::chrono::steady_clock::now()};
    double cycle_credit{};
    std::size_t display_palette{};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    bool paused{};
};

WebApp* active_app{};

std::vector<std::uint8_t> copy_browser_bytes(const emscripten::val& bytes) {
    if (bytes.isUndefined() || bytes.isNull()) return {};
    const auto size = bytes["length"].as<std::size_t>();
    std::vector<std::uint8_t> copied(size);
    if (!copied.empty()) {
        auto destination = emscripten::val(
            emscripten::typed_memory_view(copied.size(), copied.data()));
        destination.call<void>("set", bytes);
    }
    return copied;
}

emscripten::val browser_bytes(const std::vector<std::uint8_t>& bytes) {
    auto result = emscripten::val::global("Uint8Array").new_(bytes.size());
    if (!bytes.empty()) {
        result.call<void>(
            "set", emscripten::val(
                       emscripten::typed_memory_view(bytes.size(), bytes.data())));
    }
    return result;
}

void set_status(const std::string& message, const bool error = false) {
    EM_ASM({
        if (Module.gbbSetStatus) {
            Module.gbbSetStatus(UTF8ToString($0), Boolean($1));
        }
    }, message.c_str(), error);
}

void release_all_buttons(WebApp& app) {
    if (!app.emulator) return;
    for (const auto button : button_order) app.emulator->set_button(button, false);
}

gameboy::Button keyboard_button(const SDL_Keycode key, bool& matched) {
    matched = true;
    switch (key) {
    case SDLK_RIGHT: return gameboy::Button::right;
    case SDLK_LEFT: return gameboy::Button::left;
    case SDLK_UP: return gameboy::Button::up;
    case SDLK_DOWN: return gameboy::Button::down;
    case SDLK_X: return gameboy::Button::a;
    case SDLK_Z: return gameboy::Button::b;
    case SDLK_BACKSPACE: return gameboy::Button::select;
    case SDLK_RETURN: return gameboy::Button::start;
    default:
        matched = false;
        return gameboy::Button::a;
    }
}

bool gamepad_button(const Uint8 raw, gameboy::Button& button) {
    switch (static_cast<SDL_GamepadButton>(raw)) {
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: button = gameboy::Button::right; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: button = gameboy::Button::left; break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: button = gameboy::Button::up; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: button = gameboy::Button::down; break;
    case SDL_GAMEPAD_BUTTON_SOUTH: button = gameboy::Button::a; break;
    case SDL_GAMEPAD_BUTTON_EAST: button = gameboy::Button::b; break;
    case SDL_GAMEPAD_BUTTON_BACK: button = gameboy::Button::select; break;
    case SDL_GAMEPAD_BUTTON_START: button = gameboy::Button::start; break;
    default: return false;
    }
    return true;
}

void submit_audio(WebApp& app) {
    if (!app.emulator) return;
    const auto samples = app.emulator->take_audio_samples();
    if (!app.audio_stream || samples.empty()) return;

    constexpr auto maximum_queued_bytes =
        static_cast<int>(gameboy::Apu::sample_rate * sizeof(std::int16_t));
    if (SDL_GetAudioStreamQueued(app.audio_stream) > maximum_queued_bytes) {
        static_cast<void>(SDL_ClearAudioStream(app.audio_stream));
    }
    if (!SDL_PutAudioStreamData(
            app.audio_stream, samples.data(),
            static_cast<int>(samples.size() * sizeof(samples.front())))) {
        set_status(std::string{"Audio error: "} + SDL_GetError(), true);
    }
}

void present(WebApp& app) {
    static_cast<void>(SDL_SetRenderDrawColor(app.renderer, 16, 20, 16, 255));
    static_cast<void>(SDL_RenderClear(app.renderer));
    if (app.emulator) {
        const auto& pixels = app.emulator->framebuffer();
        const auto& palette = gameboy::display_palettes[app.display_palette];
        const auto native_colors = app.emulator->bus().cgb_mode() ||
                                   palette.cgb_compatibility;
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            auto pixel = native_colors
                             ? pixels[index]
                             : gameboy::apply_display_palette(pixels[index],
                                                              palette);
            if (app.video_mode == gameboy::VideoMode::lcd_shader) {
                pixel = gameboy::apply_lcd_shader(
                    pixel, index % gameboy::Ppu::screen_width,
                    index / gameboy::Ppu::screen_width);
            }
            app.display_pixels[index] = pixel;
        }
        static_cast<void>(SDL_UpdateTexture(
            app.texture, nullptr, app.display_pixels.data(),
            static_cast<int>(gameboy::Ppu::screen_width * sizeof(std::uint32_t))));
        static_cast<void>(
            SDL_RenderTexture(app.renderer, app.texture, nullptr, nullptr));
    }
    static_cast<void>(SDL_RenderPresent(app.renderer));
}

void destroy(WebApp* app) {
    if (!app) return;
    delete app;
    active_app = nullptr;
    SDL_Quit();
}

} // namespace

int load_rom_from_browser(emscripten::val bytes) noexcept {
    if (!active_app || bytes.isUndefined() || bytes.isNull()) return 0;
    try {
        auto rom = copy_browser_bytes(bytes);
        if (rom.empty()) return 0;
        active_app->emulator = std::make_unique<gameboy::Emulator>(
            gameboy::Cartridge(std::move(rom)));
        // The browser has no physical printer, but it still needs to expose
        // the Game Boy Printer protocol so camera and other printer-enabled
        // games can complete their print jobs. Completed pages are drained
        // through the JavaScript binding below.
        active_app->emulator->bus().connect_printer();
        active_app->emulator->set_dmg_compatibility_colors(
            gameboy::display_palettes[active_app->display_palette]
                .cgb_compatibility);
        // Browser storage is asynchronous. Remain paused until JavaScript has
        // restored battery RAM and RTC data for this ROM.
        active_app->paused = true;
        active_app->cycle_credit = 0.0;
        active_app->previous_time = std::chrono::steady_clock::now();
        if (active_app->audio_stream) {
            static_cast<void>(SDL_ClearAudioStream(active_app->audio_stream));
        }
        return 1;
    } catch (const std::exception& error) {
        set_status(error.what(), true);
        return 0;
    }
}

int load_rom_from_browser_with_palette(emscripten::val bytes,
                                       const unsigned palette) noexcept {
    if (!active_app || palette >= gameboy::display_palettes.size()) return 0;
    active_app->display_palette = palette;
    return load_rom_from_browser(std::move(bytes));
}

std::string browser_rom_fingerprint() {
    if (!active_app || !active_app->emulator) return {};
    std::ostringstream fingerprint;
    fingerprint << std::hex << std::setw(16) << std::setfill('0')
                << active_app->emulator->rom_fingerprint();
    return fingerprint.str();
}

bool browser_has_battery() noexcept {
    return active_app && active_app->emulator &&
           active_app->emulator->has_battery();
}

bool browser_has_rtc() noexcept {
    return active_app && active_app->emulator && active_app->emulator->has_rtc();
}

bool browser_has_camera() noexcept {
    return active_app && active_app->emulator &&
           active_app->emulator->has_camera();
}

void set_browser_camera_frame(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator ||
        !active_app->emulator->has_camera()) {
        throw std::runtime_error("No Game Boy Camera ROM is loaded");
    }
    auto frame = copy_browser_bytes(bytes);
    constexpr auto expected_size = gameboy::Cartridge::camera_width *
                                   gameboy::Cartridge::camera_height;
    if (frame.size() != expected_size) {
        throw std::invalid_argument("Invalid Game Boy Camera frame size");
    }
    active_app->emulator->set_camera_frame(frame.data(), frame.size());
}

emscripten::val export_browser_save_ram() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->export_battery_ram());
}

emscripten::val export_browser_save_data() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->export_battery_save());
}

emscripten::val export_browser_state() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->save_state());
}

void import_browser_save_ram(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->import_battery_ram(copy_browser_bytes(bytes));
}

void import_browser_save_data(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->import_battery_save(copy_browser_bytes(bytes));
}

void import_browser_state(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->load_state(copy_browser_bytes(bytes));
}

emscripten::val export_browser_rtc_data() {
    if (!active_app || !active_app->emulator) {
        return emscripten::val::global("Uint8Array").new_(0);
    }
    return browser_bytes(active_app->emulator->export_rtc_data());
}

emscripten::val take_browser_printer_images() {
    auto result = emscripten::val::array();
    if (!active_app || !active_app->emulator) return result;

    for (const auto& image : active_app->emulator->bus().take_printer_images()) {
        result.call<void>("push", browser_bytes(gameboy::encode_printer_bmp(image)));
    }
    return result;
}

void import_browser_rtc_data(const emscripten::val bytes) {
    if (!active_app || !active_app->emulator) {
        throw std::runtime_error("No ROM is loaded");
    }
    active_app->emulator->import_rtc_data(copy_browser_bytes(bytes));
}

EMSCRIPTEN_BINDINGS(gbb_web_bindings) {
    emscripten::function("loadRom", &load_rom_from_browser);
    emscripten::function("loadRomWithPalette",
                         &load_rom_from_browser_with_palette);
    emscripten::function("romFingerprint", &browser_rom_fingerprint);
    emscripten::function("hasBattery", &browser_has_battery);
    emscripten::function("hasRtc", &browser_has_rtc);
    emscripten::function("hasCamera", &browser_has_camera);
    emscripten::function("setCameraFrame", &set_browser_camera_frame);
    emscripten::function("exportSaveRam", &export_browser_save_ram);
    emscripten::function("importSaveRam", &import_browser_save_ram);
    emscripten::function("exportSaveData", &export_browser_save_data);
    emscripten::function("importSaveData", &import_browser_save_data);
    emscripten::function("exportState", &export_browser_state);
    emscripten::function("importState", &import_browser_state);
    emscripten::function("exportRtcData", &export_browser_rtc_data);
    emscripten::function("importRtcData", &import_browser_rtc_data);
    emscripten::function("takePrinterImages", &take_browser_printer_images);
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_resume_audio() noexcept {
    if (active_app && active_app->audio_stream) {
        static_cast<void>(
            SDL_ResumeAudioStreamDevice(active_app->audio_stream));
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_start_rom() noexcept {
    if (!active_app || !active_app->emulator) return;
    active_app->paused = false;
    active_app->cycle_credit = 0.0;
    active_app->previous_time = std::chrono::steady_clock::now();
    if (active_app->audio_stream) {
        static_cast<void>(
            SDL_ResumeAudioStreamDevice(active_app->audio_stream));
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_pause_rom() noexcept {
    if (!active_app || !active_app->emulator) return;
    active_app->paused = true;
    release_all_buttons(*active_app);
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_set_palette(
    const unsigned palette) noexcept {
    if (active_app && palette < gameboy::display_palettes.size()) {
        active_app->display_palette = palette;
        if (active_app->emulator) {
            active_app->emulator->set_dmg_compatibility_colors(
                gameboy::display_palettes[palette].cgb_compatibility);
        }
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_set_video_mode(
    const unsigned mode) noexcept {
    if (!active_app || mode >= gameboy::video_modes.size()) return;
    active_app->video_mode = gameboy::video_modes[mode].mode;
    const auto presentation = active_app->video_mode == gameboy::VideoMode::integer
                                  ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE
                                  : SDL_LOGICAL_PRESENTATION_LETTERBOX;
    const auto filtering = active_app->video_mode == gameboy::VideoMode::bilinear
                               ? SDL_SCALEMODE_LINEAR
                               : SDL_SCALEMODE_NEAREST;
    static_cast<void>(SDL_SetRenderLogicalPresentation(
        active_app->renderer, static_cast<int>(gameboy::Ppu::screen_width),
        static_cast<int>(gameboy::Ppu::screen_height), presentation));
    static_cast<void>(SDL_SetTextureScaleMode(active_app->texture, filtering));
}

SDL_AppResult SDL_AppInit(void** appstate, int, char**) {
    auto app = std::make_unique<WebApp>();
    if (!SDL_SetAppMetadata("Go Bigger Boy (GBB)", "0.2.0",
                            "go-bigger-boy") ||
        !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        SDL_Log("Could not initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    app->window = SDL_CreateWindow("Go Bigger Boy (GBB)", 640, 576,
                                   SDL_WINDOW_RESIZABLE);
    if (!app->window) return SDL_APP_FAILURE;
    app->renderer = SDL_CreateRenderer(app->window, nullptr);
    if (!app->renderer) return SDL_APP_FAILURE;
    static_cast<void>(SDL_SetRenderVSync(app->renderer, 1));
    if (!SDL_SetRenderLogicalPresentation(
            app->renderer, static_cast<int>(gameboy::Ppu::screen_width),
            static_cast<int>(gameboy::Ppu::screen_height),
            SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        return SDL_APP_FAILURE;
    }
    app->texture = SDL_CreateTexture(
        app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        static_cast<int>(gameboy::Ppu::screen_width),
        static_cast<int>(gameboy::Ppu::screen_height));
    if (!app->texture ||
        !SDL_SetTextureScaleMode(app->texture, SDL_SCALEMODE_NEAREST)) {
        return SDL_APP_FAILURE;
    }

    const SDL_AudioSpec audio_spec{
        SDL_AUDIO_S16, 2, static_cast<int>(gameboy::Apu::sample_rate)};
    app->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
    if (!app->audio_stream) {
        SDL_Log("Audio output is unavailable: %s", SDL_GetError());
    }

    active_app = app.get();
    *appstate = app.release();
    set_status("Ready. Choose a Game Boy ROM to begin.");
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event) {
    auto& app = *static_cast<WebApp*>(appstate);
    switch (event->type) {
    case SDL_EVENT_QUIT:
        return SDL_APP_SUCCESS;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (event->key.repeat) break;
        if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_SPACE &&
            app.emulator) {
            app.paused = !app.paused;
            release_all_buttons(app);
            set_status(app.paused ? "Paused." : "Running.");
            break;
        }
        bool matched{};
        const auto button = keyboard_button(event->key.key, matched);
        if (matched && app.emulator) {
            app.emulator->set_button(button,
                                     event->type == SDL_EVENT_KEY_DOWN);
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release_all_buttons(app);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!app.gamepad) app.gamepad = SDL_OpenGamepad(event->gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (app.gamepad && SDL_GetGamepadID(app.gamepad) == event->gdevice.which) {
            SDL_CloseGamepad(app.gamepad);
            app.gamepad = nullptr;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        gameboy::Button button{};
        if (app.emulator && gamepad_button(event->gbutton.button, button)) {
            app.emulator->set_button(
                button, event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        }
        break;
    }
    default:
        break;
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate) {
    auto& app = *static_cast<WebApp*>(appstate);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::min(
        std::chrono::duration<double>(now - app.previous_time).count(), 0.1);
    app.previous_time = now;

    if (app.emulator && !app.paused) {
        app.cycle_credit = std::min(
            app.cycle_credit + elapsed * cpu_frequency, cycles_per_frame * 2.0);
        while (app.cycle_credit >= 4.0) {
            const auto cycles = app.emulator->step();
            app.cycle_credit -= static_cast<double>(cycles);
            if (app.emulator->frame_ready()) app.emulator->consume_frame();
        }
        submit_audio(app);
    }
    present(app);
    return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult) {
    destroy(static_cast<WebApp*>(appstate));
}
