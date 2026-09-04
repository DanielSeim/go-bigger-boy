#pragma once

#include "gbb/core.hpp"

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace gbb {

struct CoreLoadOptions {
    std::filesystem::path source_path;
    std::filesystem::path persistence_path;
};

struct CoreProbeResult {
    int confidence{};
    SystemId system{SystemId::unknown};
};

// A probe result paired with the factory that produced it.  Keeping this
// small, non-owning view lets frontends and diagnostics explain why a core was
// selected without exposing factory implementation details.
struct CoreProbeMatch {
    std::string_view core_id;
    CoreProbeResult result{};
};

using CoreProbe = CoreProbeResult (*)(const std::vector<std::uint8_t>&,
                                      const CoreLoadOptions&) noexcept;
using CoreCreate = std::unique_ptr<EmulatorCore> (*)(
    std::vector<std::uint8_t>, const CoreLoadOptions&);

struct CoreFactory {
    std::string_view core_id;
    std::string_view core_name;
    CoreProbe probe{};
    CoreCreate create{};
};

class CoreRegistry {
public:
    CoreRegistry() = default;
    explicit CoreRegistry(std::vector<CoreFactory> factories);

    void register_factory(CoreFactory factory);
    [[nodiscard]] const std::vector<CoreFactory>& factories() const noexcept;
    [[nodiscard]] std::vector<CoreProbeMatch> probe_matches(
        const std::vector<std::uint8_t>& rom,
        const CoreLoadOptions& options = {}) const;
    [[nodiscard]] CoreProbeResult probe(
        const std::vector<std::uint8_t>& rom,
        const CoreLoadOptions& options = {}) const noexcept;
    [[nodiscard]] std::unique_ptr<EmulatorCore> create(
        std::vector<std::uint8_t> rom,
        const CoreLoadOptions& options = {}) const;

private:
    std::vector<CoreFactory> factories_;
};

// The application registry contains every core contributed by this build.
// The contribution list is kept separate from probing and selection policy.
[[nodiscard]] const CoreRegistry& built_in_core_registry();

[[nodiscard]] std::unique_ptr<EmulatorCore> create_core(
    std::vector<std::uint8_t> rom, const CoreLoadOptions& options = {});
[[nodiscard]] std::unique_ptr<EmulatorCore> create_core_from_file(
    const std::filesystem::path& path,
    const CoreLoadOptions& options = {});

} // namespace gbb
