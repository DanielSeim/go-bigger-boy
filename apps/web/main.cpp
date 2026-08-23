#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "gameboy/emulator.hpp"

#include <emscripten.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
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
    std::chrono::steady_clock::time_point previous_time{
        std::chrono::steady_clock::now()};
    double cycle_credit{};
    bool paused{};
};

WebApp* active_app{};

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
        static_cast<void>(SDL_UpdateTexture(
            app.texture, nullptr, pixels.data(),
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

extern "C" EMSCRIPTEN_KEEPALIVE int gbb_load_rom(
    const std::uint8_t* bytes, const std::size_t size) noexcept {
    if (!active_app || !bytes || size == 0) return 0;
    try {
        std::vector<std::uint8_t> rom(bytes, bytes + size);
        active_app->emulator = std::make_unique<gameboy::Emulator>(
            gameboy::Cartridge(std::move(rom)));
        active_app->paused = false;
        active_app->cycle_credit = 0.0;
        active_app->previous_time = std::chrono::steady_clock::now();
        if (active_app->audio_stream) {
            static_cast<void>(SDL_ClearAudioStream(active_app->audio_stream));
            static_cast<void>(
                SDL_ResumeAudioStreamDevice(active_app->audio_stream));
        }
        set_status("ROM loaded. Click the screen if keyboard input is inactive.");
        return 1;
    } catch (const std::exception& error) {
        set_status(error.what(), true);
        return 0;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void gbb_resume_audio() noexcept {
    if (active_app && active_app->audio_stream) {
        static_cast<void>(
            SDL_ResumeAudioStreamDevice(active_app->audio_stream));
    }
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
