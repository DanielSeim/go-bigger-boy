#pragma once

#include "gbb/scene.hpp"

#include <cstdint>

namespace gameboy {
class Emulator;
}

namespace gbb {

// Populate a reusable generic scene snapshot from the GB/GBC adapter. Callers
// should retain the snapshot and pass it back on subsequent calls so its
// vectors stay allocated between frames.
void populate_gameboy_scene_snapshot(const gameboy::Emulator& emulator,
                                     SceneSnapshot& scene);

// Return a compact signature of the emulated inputs that can affect the
// renderer. Tile provenance is optional because the standard and shape-aware
// reliefs only depend on the exact framebuffer plus OAM/window metadata.
// This avoids rebuilding and allocating the full scene snapshot on cache hits.
[[nodiscard]] std::uint64_t gameboy_scene_input_signature(
    const gameboy::Emulator& emulator,
    bool include_tile_provenance = false) noexcept;

} // namespace gbb
