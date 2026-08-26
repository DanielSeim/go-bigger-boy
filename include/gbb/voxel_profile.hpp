#pragma once

#include <cstdint>
#include <filesystem>

namespace gbb {

// Presentation-only tuning for the optional voxel diorama renderer. The
// profile is intentionally independent from any emulation core and is keyed
// by a ROM fingerprint by the frontend.
struct VoxelProfile {
    float depth_scale{1.0F};
    float camera_pitch{28.0F};
    float camera_yaw{32.0F};
    float zoom{1.0F};
    float perspective{0.0025F};
    float sprite_depth{10.0F};
    float lighting{1.0F};
    bool framebuffer_facade{true};
};

VoxelProfile load_voxel_profile(const std::filesystem::path& path,
                                std::uint64_t fingerprint);
bool save_voxel_profile(const std::filesystem::path& path,
                        std::uint64_t fingerprint,
                        const VoxelProfile& profile);
void ensure_voxel_profile_file(const std::filesystem::path& path);

} // namespace gbb
