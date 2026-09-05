#pragma once

#include "gbb/core_registry.hpp"
#include "gbb/plugin_abi.h"

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
    // Capability IDs are an explicit permission policy. An empty list grants
    // no optional capabilities when the requirement flag is enabled.
    std::vector<std::string> allowed_capability_ids;
    bool require_capability_allowlist{};
    // Optional synchronous trust decision after descriptor validation and
    // before registration. The descriptor reference is only valid during the
    // callback; returning false rejects the plugin. Callbacks must not retain
    // the reference or throw.
    std::function<bool(const std::filesystem::path&,
                       const gbb_plugin_descriptor_v1&)> trust_callback;
    std::size_t max_plugins{32};
};

// Stable configuration names for the capability bits in plugin_abi.h. Unknown
// names are rejected by PluginCatalog rather than silently broadening trust.
[[nodiscard]] std::uint64_t plugin_capability_bit(
    std::string_view id) noexcept;

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
