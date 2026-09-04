#pragma once

#include "gameboy/display_palette.hpp"
#include "gameboy/video_pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gbb {

// Applies the shared framebuffer presentation transforms used by SDL and the
// browser. The emulator remains responsible for producing native pixels;
// this function owns only palette mapping and optional post-processing.
void transform_video_frame(const std::uint32_t* source,
                           std::size_t pixel_count, std::size_t width,
                           std::size_t height,
                           const gameboy::DisplayPalette& palette,
                           bool native_colors, gameboy::VideoMode mode,
                           std::vector<std::uint32_t>& destination);

} // namespace gbb
