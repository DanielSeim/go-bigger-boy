#pragma once

#include "gbb/core.hpp"
#include "gbb/core_registry.hpp"
#include "gbb/plugin_abi.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gbb {

// Native dynamic-core loader. Loading is explicit and never part of the
// built-in registry; callers must opt in to a library path and handle errors.
class PluginLoader final : public std::enable_shared_from_this<PluginLoader> {
public:
    [[nodiscard]] static std::shared_ptr<PluginLoader> load(
        const std::filesystem::path& path, std::string& error);

    ~PluginLoader();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    [[nodiscard]] const gbb_plugin_descriptor_v1& descriptor() const noexcept;
    [[nodiscard]] std::unique_ptr<EmulatorCore> create(
        std::vector<std::uint8_t> rom, const CoreLoadOptions& options = {}) const;

private:
    PluginLoader(void* library, gbb_plugin_v1 plugin,
                 std::filesystem::path path) noexcept;

    void* library_{};
    gbb_plugin_v1 plugin_{};
    std::filesystem::path path_;
};

} // namespace gbb
