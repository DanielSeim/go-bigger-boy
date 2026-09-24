#pragma once

#include "audio_output.hpp"
#include "camera_capture.hpp"
#include "settings_model.hpp"

#include "gameboy/emulator.hpp"
#include "gbb/video.hpp"
#include "gbb/scene.hpp"
#include "gbb/voxel_profile.hpp"
#include "gbb/voxel_scene.hpp"
#include "frame_rate_metrics.hpp"

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
    // SGB voxel presentation draws only border slices from this texture, so
    // the live native viewport does not force a full 256x224 upload.
    SDL_Texture* sgb_border_texture{};
    // Opaque border pixels inside the GB window are drawn over voxel output.
    SDL_Texture* sgb_viewport_overlay_texture{};
    // Voxel geometry always samples the native 160x144 Game Boy image. Keep
    // this separate from the 256x224 SGB presentation texture.
    SDL_Texture* voxel_texture{};
    SDL_Texture* voxel_render_target{};
    std::size_t core_video_width{160};
    std::size_t core_video_height{144};
    gameboy::VideoMode video_mode{gameboy::default_video_mode};
    gbb::SceneSnapshot scene_snapshot{};
    std::filesystem::path voxel_profile_path;
    gbb::VoxelProfile voxel_profile{};
    std::uint64_t voxel_profile_fingerprint{};
    bool voxel_profile_loaded{};
    gbb::VoxelScene voxel_scene{};
    std::uint64_t voxel_scene_signature{};
    bool voxel_scene_cached{};
    gbb::VoxelRenderStats voxel_stats{};
    SDL_Gamepad* gamepad{};
    AudioOutput audio;
    CameraCapture camera;
    bool rumble_output_active{};
    bool rumble_warning_shown{};
    std::chrono::steady_clock::time_point rumble_refresh{};
    std::vector<SDL_Vertex> voxel_vertices;
    std::vector<int> voxel_indices;
    std::vector<std::uint32_t> presentation_pixels;
    float voxel_camera_pitch_offset{};
    float voxel_camera_yaw_offset{};
    std::uint64_t voxel_render_cache_key{};
    std::uint64_t voxel_render_cache_state_key{};
    bool voxel_render_cache_valid{};
    std::vector<std::uint32_t> voxel_render_cache_source_pixels;
    std::uint64_t sgb_border_texture_key{};
    std::uint64_t sgb_border_source_key{};
    bool sgb_border_texture_valid{};
    bool sgb_viewport_overlay_visible{};
    bool voxel_camera_dragging{};
    bool split_screen{};
    FrameRateMetrics fps_metrics{};
    std::uint32_t fps_log_windows{};
    float fps_value{};
#ifdef __ANDROID__
    std::uint64_t sgb_compose_us{};
    std::uint64_t sgb_transform_us{};
    std::uint64_t sgb_upload_us{};
    struct TouchPoint {
        SDL_FingerID id{};
        float x{};
        float y{};
        bool orbit{};
        // Capture the overlay hit on finger-down. Android can report a
        // slightly different release coordinate after touch slop, which must
        // not turn the hamburger button into the adjacent link button.
        std::uint8_t overlay_button{};
        std::optional<std::size_t> control;
        std::optional<std::size_t> secondary_control;
        std::uint8_t secondary_neutral_motion_count{};
    };
    std::vector<TouchPoint> touches;
    std::array<bool, 8> touch_buttons{};
    bool android_menu_visible{};
    bool android_link_menu_visible{};
    SDL_Texture* touch_overlay_texture{};
    int touch_overlay_width{};
    int touch_overlay_height{};
    std::uint64_t touch_overlay_cache_key{};
    bool touch_overlay_cache_valid{};
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
