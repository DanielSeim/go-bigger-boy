#pragma once

#include "gbb/scene.hpp"

namespace gameboy {
class Emulator;
}

namespace gbb {

// Populate a reusable generic scene snapshot from the GB/GBC adapter. Callers
// should retain the snapshot and pass it back on subsequent calls so its
// vectors stay allocated between frames.
void populate_gameboy_scene_snapshot(const gameboy::Emulator& emulator,
                                     SceneSnapshot& scene);

} // namespace gbb
