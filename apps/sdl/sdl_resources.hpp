#pragma once

#include "audio_output.hpp"
#include "camera_capture.hpp"
#include "settings_model.hpp"

#include "gameboy/emulator.hpp"
#include "gbb/video.hpp"
#include "gbb/scene.hpp"
#include "gbb/voxel_profile.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace gbb::sdl {

class SdlResources {
  public:
    explicit SdlResources(std::string_view version,
                          bool initially_hidden = false);
    ~SdlResources();

    SdlResources(const SdlResources&) = delete;
    SdlResources& operator=(const SdlResources&) = delete;

    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Texture* link_texture{};
    std::size_t core_video_width{160};
    std::size_t core_video_height{144};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    gbb::SceneSnapshot scene_snapshot{};
    std::filesystem::path voxel_profile_path;
    gbb::VoxelProfile voxel_profile{};
    std::uint64_t voxel_profile_fingerprint{};
    bool voxel_profile_loaded{};
    SDL_Gamepad* gamepad{};
    AudioOutput audio;
    CameraCapture camera;
    bool rumble_output_active{};
    bool rumble_warning_shown{};
    std::chrono::steady_clock::time_point rumble_refresh{};
    std::vector<SDL_Vertex> voxel_vertices;
    std::vector<int> voxel_indices;
    float voxel_camera_pitch_offset{};
    float voxel_camera_yaw_offset{};
    bool voxel_camera_dragging{};
    bool split_screen{};
#ifdef __ANDROID__
    struct TouchPoint {
        SDL_FingerID id{};
        float x{};
        float y{};
        bool orbit{};
        std::optional<std::size_t> control;
    };
    std::vector<TouchPoint> touches;
    std::array<bool, 8> touch_buttons{};
    TouchControlSettings touch_settings;
    std::filesystem::file_time_type touch_settings_write_time{};
    bool touch_settings_write_time_valid{};
    std::filesystem::file_time_type video_settings_write_time{};
    bool video_settings_write_time_valid{};
    std::filesystem::file_time_type palette_settings_write_time{};
    bool palette_settings_write_time_valid{};
#endif

  private:
    void release() noexcept;
    bool sdl_initialized_{};
};

} // namespace gbb::sdl
