#include "gbb/plugin_discovery.hpp"

#include "gbb/log.hpp"
#include "gbb/core_contributors.hpp"
#include "gbb/plugin_loader.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

namespace gbb {
namespace {

bool supported_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                   });
#if defined(_WIN32)
    return extension == ".dll";
#elif defined(__APPLE__)
    return extension == ".dylib" || extension == ".so";
#else
    return extension == ".so";
#endif
}

void add_candidate(const std::filesystem::path& path,
                   std::vector<std::filesystem::path>& candidates,
                   std::set<std::filesystem::path>& seen,
                   std::vector<PluginDiagnostic>& diagnostics) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        diagnostics.push_back({path, false, "path does not exist"});
        return;
    }
    if (std::filesystem::is_symlink(path, error)) {
        diagnostics.push_back({path, false,
                               "symbolic links are not allowed for plugins"});
        return;
    }
    if (error) {
        diagnostics.push_back({path, false,
                               "could not inspect plugin path: " +
                                   error.message()});
        return;
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
        diagnostics.push_back({path, false, "path is not a regular file"});
        return;
    }
    if (!supported_extension(path)) {
        diagnostics.push_back({path, false, "unsupported plugin filename"});
        return;
    }
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    const auto key = error ? path.lexically_normal() : canonical;
    if (seen.insert(key).second) candidates.push_back(key);
}

} // namespace

PluginCatalog PluginCatalog::discover(const PluginDiscoveryOptions& options) {
    PluginCatalog catalog;
    catalog.registry_ = CoreRegistry{built_in_core_factories()};
    if (!options.enabled) {
        Logger::instance().write(LogLevel::info, LogCategory::core,
                                 "plugin discovery disabled (opt-in)");
        return catalog;
    }
    if (options.paths.empty()) {
        catalog.diagnostics_.push_back(
            {{}, false, "plugin discovery enabled but no paths were configured"});
        Logger::instance().write(
            LogLevel::warning, LogCategory::core,
            "plugin discovery enabled but no paths were configured");
        return catalog;
    }

    const auto limit = std::min<std::size_t>(options.max_plugins, 256);
    std::vector<std::filesystem::path> candidates;
    std::set<std::filesystem::path> seen;
    bool limit_reached = limit == 0;
    for (const auto& path : options.paths) {
        std::error_code error;
        if (std::filesystem::is_symlink(path, error)) {
            catalog.diagnostics_.push_back(
                {path, false, "symbolic links are not allowed for plugins"});
            continue;
        }
        if (error) {
            catalog.diagnostics_.push_back(
                {path, false, "could not inspect plugin path: " +
                                  error.message()});
            continue;
        }
        if (std::filesystem::is_directory(path, error) && !error) {
            std::vector<std::filesystem::path> entries;
            for (const auto& entry :
                 std::filesystem::directory_iterator(path, error)) {
                if (error) break;
                entries.push_back(entry.path());
            }
            std::sort(entries.begin(), entries.end());
            for (const auto& entry : entries) {
                if (candidates.size() >= limit) {
                    limit_reached = true;
                    break;
                }
                add_candidate(entry, candidates, seen, catalog.diagnostics_);
            }
            if (error) {
                catalog.diagnostics_.push_back(
                    {path, false, "could not enumerate plugin directory: " +
                                  error.message()});
            }
            continue;
        }
        if (candidates.size() < limit) {
            add_candidate(path, candidates, seen, catalog.diagnostics_);
        } else {
            limit_reached = true;
        }
    }
    if (limit_reached) {
        catalog.diagnostics_.push_back(
            {{}, false, "plugin discovery limit reached"});
    }

    for (const auto& path : candidates) {
        std::string error;
        auto loader = PluginLoader::load(path, error);
        if (!loader) {
            const auto message = error.empty() ? "plugin rejected" : error;
            catalog.diagnostics_.push_back({path, false, message});
            Logger::instance().write(
                LogLevel::warning, LogCategory::core,
                std::string("plugin rejected path=") + path.string() +
                    " reason=" + message);
            continue;
        }
        try {
            const auto& descriptor = loader->descriptor();
            const auto allowed = std::find(options.allowed_core_ids.begin(),
                                           options.allowed_core_ids.end(),
                                           descriptor.core_id) !=
                                 options.allowed_core_ids.end();
            if (options.require_allowlist && !allowed) {
                const auto message = std::string("core id is not in the "
                                                 "plugin allowlist: ") +
                                     descriptor.core_id;
                catalog.diagnostics_.push_back({path, false, message});
                Logger::instance().write(
                    LogLevel::warning, LogCategory::core,
                    std::string("plugin rejected path=") + path.string() +
                        " reason=" + message);
                continue;
            }
            if (!options.allowed_core_ids.empty() && !allowed) {
                const auto message = std::string("core id is not allowlisted: ") +
                                     descriptor.core_id;
                catalog.diagnostics_.push_back({path, false, message});
                Logger::instance().write(
                    LogLevel::warning, LogCategory::core,
                    std::string("plugin rejected path=") + path.string() +
                        " reason=" + message);
                continue;
            }
            catalog.registry_.register_plugin(loader);
            catalog.loaders_.push_back(std::move(loader));
            const auto message = std::string("loaded plugin core=") +
                                 descriptor.core_id;
            catalog.diagnostics_.push_back({path, true, message});
            Logger::instance().write(LogLevel::info, LogCategory::core,
                                     message + " path=" + path.string());
        } catch (const std::exception& exception) {
            catalog.diagnostics_.push_back({path, false, exception.what()});
            Logger::instance().write(
                LogLevel::warning, LogCategory::core,
                std::string("plugin rejected path=") + path.string() +
                    " reason=" + exception.what());
        }
    }
    return catalog;
}

} // namespace gbb
