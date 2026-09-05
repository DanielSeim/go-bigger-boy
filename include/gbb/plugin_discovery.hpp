#pragma once

#include "gbb/core_registry.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gbb {

// Plugin loading is deliberately opt-in. A caller must set enabled and supply
// one or more paths; no implicit working-directory or system-directory scan is
// ever performed.
struct PluginDiscoveryOptions {
    bool enabled{};
    std::vector<std::filesystem::path> paths;
    // When non-empty, only descriptors with an exact matching core_id are
    // trusted. This is an identity allowlist, not a signature mechanism.
    std::vector<std::string> allowed_core_ids;
    bool require_allowlist{};
    std::size_t max_plugins{32};
};

struct PluginDiagnostic {
    std::filesystem::path path;
    bool loaded{};
    std::string message;
};

class PluginCatalog final {
public:
    PluginCatalog() = default;

    [[nodiscard]] static PluginCatalog discover(
        const PluginDiscoveryOptions& options);

    [[nodiscard]] const CoreRegistry& registry() const noexcept {
        return registry_;
    }
    [[nodiscard]] const std::vector<PluginDiagnostic>& diagnostics() const
        noexcept {
        return diagnostics_;
    }
    [[nodiscard]] std::size_t loaded_count() const noexcept {
        return loaders_.size();
    }

private:
    CoreRegistry registry_;
    std::vector<std::shared_ptr<const PluginLoader>> loaders_;
    std::vector<PluginDiagnostic> diagnostics_;
};

} // namespace gbb
