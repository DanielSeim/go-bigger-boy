#pragma once

#include "gameboy/display_palette.hpp"
#include "gameboy/emulator.hpp"
#include "gameboy/ppu.hpp"
#include "gameboy/video_pipeline.hpp"
#include "gbb/scene.hpp"
#include "gbb/video.hpp"
#include "gbb/voxel_profile.hpp"
#include "gbb/voxel_scene.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gbb::sdl {

// The voxel renderer only needs this small presentation context. Keeping it
// independent from SdlResources lets the large SDL event/lifecycle object
// evolve without pulling rendering state through every helper.
struct VoxelRenderContext {
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    SDL_Texture* render_target{};
    gameboy::VideoMode video_mode{};
    gbb::SceneSnapshot& scene_snapshot;
    std::filesystem::path& voxel_profile_path;
    gbb::VoxelProfile& voxel_profile;
    std::uint64_t& voxel_profile_fingerprint;
    bool& voxel_profile_loaded;
    gbb::VoxelScene& voxel_scene;
    std::uint64_t& voxel_scene_signature;
    bool& voxel_scene_cached;
    gbb::VoxelRenderStats& voxel_stats;
    std::vector<SDL_Vertex>& voxel_vertices;
    std::vector<int>& voxel_indices;
    float& voxel_camera_pitch_offset;
    float& voxel_camera_yaw_offset;
    std::uint64_t& voxel_render_cache_key;
    bool& voxel_render_cache_valid;
};

[[nodiscard]] SDL_FColor voxel_color(std::uint32_t pixel, float shade,
                                      float ambient = 0.0F);

[[nodiscard]] bool render_voxel_diorama(const gameboy::Emulator& emulator,
                                         VoxelRenderContext& context,
                                         const gameboy::DisplayPalette& palette,
                                         bool shape_aware = false,
                                         bool popup_book = false);

} // namespace gbb::sdl
