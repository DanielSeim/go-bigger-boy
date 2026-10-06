#pragma once
#include "gbb/core_registry.hpp"

namespace gbb {
// Validate required caller-owned program images and optional boot overrides;
// does not execute firmware or load a game.
void validate_sgb_firmware_images(const std::filesystem::path& directory, std::string_view model);
[[nodiscard]] std::unique_ptr<EmulatorCore> create_sgb_firmware_core(
    std::vector<std::uint8_t> rom, const CoreLoadOptions& options);
// Read-only identity for segregating experimental frontend storage.
[[nodiscard]] std::string_view sgb_firmware_model(const EmulatorCore& core) noexcept;
}
