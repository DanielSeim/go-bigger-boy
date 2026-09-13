#pragma once

#include "gbb/scene.hpp"
#include "gbb/voxel_profile.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gbb {

enum class VoxelObjectKind : std::uint8_t {
    background_object,
    sprite,
};

// A resolved presentation object. `source_cells` indexes the immutable
// SceneSnapshot that was used to build the scene.
struct VoxelObject {
    std::string id;
    VoxelObjectKind kind{VoxelObjectKind::background_object};
    float confidence{};
    std::int16_t min_x{};
    std::int16_t min_y{};
    std::int16_t max_x{};
    std::int16_t max_y{};
    // The page/cut-out hinge. Objects are raised from this screen-space row.
    std::int16_t anchor_y{};
    std::vector<std::size_t> source_cells;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> map_cells;
};

struct VoxelScene {
    std::uint32_t schema_version{1};
    std::size_t width{};
    std::size_t height{};
    // -1 means flat background. Otherwise this contains an index into
    // `objects`, allowing renderers to apply one object transform to all
    // occupied pixels and making debug overlays deterministic.
    std::vector<std::int32_t> object_owner;
    std::vector<VoxelObject> objects;
    // All profile candidates are retained for diagnostics; `objects` only
    // contains candidates accepted as geometry.
    std::vector<VoxelObject> candidates;
};

struct VoxelSceneBuildOptions {
    bool include_sprites{true};
    bool detect_background_objects{false};
    std::size_t minimum_background_cells{4};
    float maximum_background_fraction{0.55F};
    float accepted_confidence{0.72F};
    const std::vector<VoxelObjectTemplate>* background_templates{};
};

// Build a presentation-only scene from hardware provenance. No emulation
// state is modified. Unknown or ambiguous background regions remain flat by
// default; callers opt into background-object detection through a ROM profile.
[[nodiscard]] VoxelScene build_voxel_scene(
    const SceneSnapshot& snapshot,
    const VoxelSceneBuildOptions& options = {});

} // namespace gbb
