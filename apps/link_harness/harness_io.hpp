#pragma once

#include "gameboy/ppu.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gbb::link_harness {

[[nodiscard]] std::vector<std::uint8_t> read_bytes(
    const std::filesystem::path& path);
[[nodiscard]] std::uint64_t fingerprint(
    const std::vector<std::uint8_t>& bytes) noexcept;
[[nodiscard]] std::string hex(std::uint64_t value);

void write_report(const std::filesystem::path& path, const std::string& report);
void write_frame(const std::filesystem::path& path,
                 const gameboy::Ppu::Framebuffer& framebuffer);

} // namespace gbb::link_harness
