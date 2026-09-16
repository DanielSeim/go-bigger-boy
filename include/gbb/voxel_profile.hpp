#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gbb {

// A compact authored tile arrangement. `tile_ids` is row-major and -1 is a
// wildcard, allowing a profile to describe a roof/wall shape while ignoring
// palette or animation variants.
struct VoxelObjectTemplate {
    std::string id;
    std::uint32_t width{2};
    std::uint32_t height{2};
    std::vector<std::int16_t> tile_ids;
};

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
    // Logical far/near bounds for the three diorama layers. Larger values
    // represent greater distance from the viewer; the renderer normalizes
    // each band and places background, window, then sprites toward the viewer.
    float background_depth_far{100.0F};
    float background_depth_near{20.0F};
    float background_transparent_depth{95.0F};
    float window_depth_far{90.0F};
    float window_depth_near{50.0F};
    float sprite_depth_far{45.0F};
    float sprite_depth_near{25.0F};
    // Pop-up book presentation tuning. These values are intentionally kept
    // in the ROM profile so visual iteration does not require a renderer
    // rebuild.
    float popup_parallax{0.86F};
    float popup_object_height{0.28F};
    float popup_sprite_height{0.86F};
    float popup_card_thickness{4.5F};
    float popup_sprite_thickness{1.65F};
    // Enable the conservative provenance-based background object detector by
    // default. It only accepts bounded, map-aligned structures; ambiguous
    // artwork remains on the page.
    bool background_object_detection{true};
    std::uint32_t background_object_min_cells{4};
    float background_object_max_fraction{0.55F};
    float background_object_confidence{0.72F};
    std::vector<VoxelObjectTemplate> background_object_templates;
    bool background_debug_overlay{false};
    // Keep the voxel mesh visible by default. The original framebuffer can
    // be enabled explicitly when a front-facing reference image is desired.
    bool framebuffer_facade{false};
};

// Return renderer defaults tuned for a known ROM fingerprint. Unknown ROMs
// receive the generic profile values above.
[[nodiscard]] VoxelProfile built_in_voxel_profile(std::uint64_t fingerprint);

VoxelProfile load_voxel_profile(const std::filesystem::path& path,
                                std::uint64_t fingerprint);
bool save_voxel_profile(const std::filesystem::path& path,
                        std::uint64_t fingerprint,
                        const VoxelProfile& profile);
void ensure_voxel_profile_file(const std::filesystem::path& path);

} // namespace gbb
