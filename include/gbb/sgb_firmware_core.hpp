#pragma once
#include "gbb/core_registry.hpp"

namespace gbb {
[[nodiscard]] std::unique_ptr<EmulatorCore> create_sgb_firmware_core(
    std::vector<std::uint8_t> rom, const CoreLoadOptions& options);
// Read-only identity for segregating experimental frontend storage.
[[nodiscard]] std::string_view sgb_firmware_model(const EmulatorCore& core) noexcept;
}
