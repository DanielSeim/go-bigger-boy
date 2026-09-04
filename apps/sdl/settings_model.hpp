#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>

// SDL-facing settings data is kept in a small model header so the dashboard,
// Android JNI bridge, and settings persistence code share one contract. The
// parser and file I/O remain separate from this data-only definition.

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

struct TouchControlSettings {
    float scale{1.35F};
    float opacity{0.78F};
    bool voxel_orbit{true};
    bool menu_top_right{};
    // Window coordinates normalized to 0..1. Each orientation has one
    // movable D-pad plus A, B, Select, and Start. The first ten values are
    // portrait; the second ten are landscape. Each control stores x, y.
    std::array<float, 20> positions{
        0.27F, 0.82F, 0.74F, 0.79F, 0.74F, 0.90F, 0.43F, 0.96F,
        0.57F, 0.96F,
        0.12F, 0.50F, 0.88F, 0.42F, 0.88F, 0.62F, 0.42F, 0.92F,
        0.58F, 0.92F};
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
