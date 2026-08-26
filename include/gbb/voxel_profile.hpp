#pragma once

#include <cstdint>
#include <filesystem>

namespace gbb {

// Presentation-only tuning for the optional voxel diorama renderer. The
// profile is intentionally independent from any emulation core and is keyed
// by a ROM fingerprint by the frontend.
struct VoxelProfile {
    float depth_scale{1.0F};
    float camera_pitch{24.0F};
    float camera_yaw{0.0F};
    float zoom{0.72F};
    float perspective{0.0015F};
    float sprite_depth{8.0F};
    float lighting{1.0F};
    // Keep the voxel mesh visible by default. The original framebuffer can
    // be enabled explicitly when a front-facing reference image is desired.
    bool framebuffer_facade{false};
};

VoxelProfile load_voxel_profile(const std::filesystem::path& path,
                                std::uint64_t fingerprint);
bool save_voxel_profile(const std::filesystem::path& path,
                        std::uint64_t fingerprint,
                        const VoxelProfile& profile);
void ensure_voxel_profile_file(const std::filesystem::path& path);

} // namespace gbb
