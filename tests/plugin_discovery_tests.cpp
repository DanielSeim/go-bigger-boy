#include "gbb/plugin_discovery.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {
int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: gameboy_plugin_discovery_tests FIXTURE_LIBRARY\n";
        return 2;
    }

    const auto fixture = std::filesystem::path(argv[1]);
    const auto missing = fixture.parent_path() / "does-not-exist.so";

    const auto disabled = gbb::PluginCatalog::discover({});
    check(disabled.loaded_count() == 0 &&
              disabled.registry().factories().size() == 1,
          "discovery is disabled by default and keeps the built-in registry");

    gbb::PluginDiscoveryOptions options;
    options.enabled = true;
    options.paths = {fixture, fixture};
    const auto catalog = gbb::PluginCatalog::discover(options);
    check(catalog.loaded_count() == 1,
          "explicit duplicate plugin paths are loaded only once");
    check(catalog.registry().factories().size() == 2,
          "loaded plugin is registered beside built-in cores");
    check(!catalog.diagnostics().empty() && catalog.diagnostics().front().loaded,
          "successful plugin loading produces a diagnostic record");

    const std::vector<std::uint8_t> unsupported_by_gb{1, 2, 3, 4};
    const auto core = catalog.registry().create(unsupported_by_gb);
    check(core != nullptr && core->descriptor().core_id == "fixture",
          "registry can select an explicitly discovered plugin");

    gbb::PluginDiscoveryOptions allowlisted;
    allowlisted.enabled = true;
    allowlisted.paths = {fixture};
    allowlisted.require_allowlist = true;
    allowlisted.allowed_core_ids = {"other-core"};
    const auto denied = gbb::PluginCatalog::discover(allowlisted);
    check(denied.loaded_count() == 0 && denied.diagnostics().size() == 1 &&
              denied.diagnostics().front().message.find("allowlist") !=
                  std::string::npos,
          "required identity allowlists reject unapproved descriptors");

    allowlisted.allowed_core_ids = {"fixture"};
    const auto accepted = gbb::PluginCatalog::discover(allowlisted);
    check(accepted.loaded_count() == 1,
          "identity allowlists permit explicitly approved descriptors");

    gbb::PluginDiscoveryOptions rejected;
    rejected.enabled = true;
    rejected.paths = {missing};
    const auto rejected_catalog = gbb::PluginCatalog::discover(rejected);
    check(rejected_catalog.loaded_count() == 0 &&
              rejected_catalog.diagnostics().size() == 1 &&
              !rejected_catalog.diagnostics().front().loaded,
          "missing plugin paths are reported without changing the registry");

    // Directory symlinks must not be traversed: doing so would let an
    // explicitly trusted directory redirect discovery elsewhere.
    const auto symlink_directory =
        std::filesystem::temp_directory_path() / "gbb-plugin-discovery-link";
    std::error_code symlink_error;
    std::filesystem::remove(symlink_directory, symlink_error);
    symlink_error.clear();
    std::filesystem::create_directory_symlink(fixture.parent_path(),
                                               symlink_directory,
                                               symlink_error);
    if (!symlink_error) {
        gbb::PluginDiscoveryOptions symlink_options;
        symlink_options.enabled = true;
        symlink_options.paths = {symlink_directory};
        const auto symlink_catalog =
            gbb::PluginCatalog::discover(symlink_options);
        check(symlink_catalog.loaded_count() == 0 &&
                  symlink_catalog.diagnostics().size() == 1 &&
                  symlink_catalog.diagnostics().front().message.find(
                      "symbolic links") != std::string::npos,
              "directory symlinks are rejected before traversal");
        std::filesystem::remove(symlink_directory, symlink_error);
    }

    return failures == 0 ? 0 : 1;
}
