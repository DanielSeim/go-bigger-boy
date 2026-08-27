#pragma once

#include "gbb/scene.hpp"

#include <filesystem>
#include <string>

namespace gbb {

// Serialize a scene snapshot using the versioned, hardware-neutral GBB scene
// JSON schema. The output contains only JSON primitives and arrays so it can
// be consumed by tools without linking the emulator.
[[nodiscard]] std::string scene_snapshot_to_json(const SceneSnapshot& scene);

// Write a scene snapshot to disk. Returns false when the file cannot be
// created or written.
[[nodiscard]] bool write_scene_snapshot_json(const SceneSnapshot& scene,
                                             const std::filesystem::path& path);

} // namespace gbb
