#include "gbb/core_registry.hpp"
#include "gbb/core_contract.hpp"
#include "gbb/core_contributors.hpp"
#include "gbb/log.hpp"
#include "gbb/plugin_loader.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>

namespace gbb {

CoreRegistry::CoreRegistry(std::vector<CoreFactory> factories)
{
    factories_.reserve(factories.size());
    providers_.reserve(factories.size());
    for (const auto& factory : factories) register_factory(factory);
}

void CoreRegistry::register_factory(CoreFactory factory) {
    if (factory.probe == nullptr || factory.create == nullptr) {
        throw std::invalid_argument(
            "A core factory requires an id, name, probe, and creator");
    }
    register_provider({
        factory,
        [probe = factory.probe](const auto& rom, const auto& options) {
            return probe(rom, options);
        },
        [create = factory.create](auto rom, const auto& options) {
            return create(std::move(rom), options);
        }});
}

void CoreRegistry::register_provider(CoreProvider provider) {
    const auto metadata = provider.factory;
    const auto& factory = metadata;
    if (factory.core_id.empty() || factory.core_name.empty() ||
        !provider.probe || !provider.create) {
        throw std::invalid_argument(
            "A core factory requires an id, name, probe, and creator");
    }
    const auto duplicate = std::find_if(
        factories_.begin(), factories_.end(), [&](const CoreFactory& existing) {
            return existing.core_id == factory.core_id;
        });
    if (duplicate != factories_.end()) {
        throw std::invalid_argument("Duplicate core id: " +
                                    std::string(factory.core_id));
    }
    providers_.push_back(std::move(provider));
    try {
        factories_.push_back(metadata);
    } catch (...) {
        providers_.pop_back();
        throw;
    }
}

void CoreRegistry::register_plugin(std::shared_ptr<const PluginLoader> loader) {
    if (!loader) throw std::invalid_argument("Cannot register an empty plugin");
    const auto& descriptor = loader->descriptor();
    CoreFactory metadata{descriptor.core_id, descriptor.core_name, nullptr,
                         nullptr};
    // A v1 plug-in does not expose a host-side probe callback. Its descriptor
    // is authoritative once the user has explicitly selected the library;
    // return a deliberately low confidence so the built-in core remains the
    // deterministic default for ROMs it recognizes.
    auto probe_loader = loader;
    auto create_loader = std::move(loader);
    auto probe = [probe_loader](
                     const std::vector<std::uint8_t>& rom,
                     const CoreLoadOptions&) -> CoreProbeResult {
        if (rom.empty()) return {};
        const auto& descriptor = probe_loader->descriptor();
        const auto system = descriptor.system_id == GBB_PLUGIN_SYSTEM_GB
                                ? SystemId::game_boy
                            : descriptor.system_id == GBB_PLUGIN_SYSTEM_GBC
                                ? SystemId::game_boy_color
                            : SystemId::game_boy_advance;
        return {1, system};
    };
    // The probe lambda owns the loader; the create lambda receives its own
    // shared ownership so either callback remains valid independently.
    auto create = [create_loader](std::vector<std::uint8_t> rom,
                                  const CoreLoadOptions& options) {
        return create_loader->create(std::move(rom), options);
    };
    register_provider({metadata, std::move(probe), std::move(create)});
}

const std::vector<CoreFactory>& CoreRegistry::factories() const noexcept {
    return factories_;
}

std::vector<CoreProbeMatch> CoreRegistry::probe_matches(
    const std::vector<std::uint8_t>& rom,
    const CoreLoadOptions& options) const {
    std::vector<CoreProbeMatch> matches;
    matches.reserve(factories_.size());
    for (std::size_t index = 0; index < factories_.size(); ++index) {
        matches.push_back({factories_[index].core_id,
                           providers_[index].probe(rom, options)});
    }
    return matches;
}

CoreProbeResult CoreRegistry::probe(
    const std::vector<std::uint8_t>& rom,
    const CoreLoadOptions& options) const noexcept {
    CoreProbeResult best{};
    for (std::size_t index = 0; index < factories_.size(); ++index) {
        const auto candidate = providers_[index].probe(rom, options);
        if (candidate.confidence > best.confidence) best = candidate;
    }
    return best;
}

std::unique_ptr<EmulatorCore> CoreRegistry::create(
    std::vector<std::uint8_t> rom, const CoreLoadOptions& options) const {
    const CoreFactory* best{};
    auto confidence = 0;
    const auto matches = probe_matches(rom, options);
    auto& logger = Logger::instance();
    for (std::size_t index = 0; index < matches.size(); ++index) {
        const auto& match = matches[index];
        if (logger.enabled(LogLevel::debug)) {
            const auto message = std::string("probe core=") +
                std::string(match.core_id) + " confidence=" +
                std::to_string(match.result.confidence) + " system=" +
                std::string(system_id_string(match.result.system));
            logger.write(LogLevel::debug, LogCategory::core, message);
        }
        if (match.result.confidence > confidence) {
            confidence = match.result.confidence;
            best = &factories_[index];
        } else if (best != nullptr && match.result.confidence > 0 &&
                   match.result.confidence == confidence) {
            const auto message = std::string("probe tie core=") +
                std::string(match.core_id) + " confidence=" +
                std::to_string(confidence) + " keeping core=" +
                std::string(best->core_id);
            logger.write(LogLevel::warning, LogCategory::core, message);
        }
    }
    if (best == nullptr) {
        logger.write(LogLevel::error, LogCategory::core,
                     "no core recognized the supplied ROM");
        throw std::runtime_error("No installed emulator core recognizes this ROM");
    }
    if (logger.enabled(LogLevel::info)) {
        const auto message = std::string("selected core=") +
            std::string(best->core_id) + " confidence=" +
            std::to_string(confidence);
        logger.write(LogLevel::info, LogCategory::core, message);
    }
    const auto best_index = static_cast<std::size_t>(best - factories_.data());
    auto core = providers_[best_index].create(std::move(rom), options);
    if (!core) {
        const auto message = std::string("core factory returned no instance: ") +
                             std::string(best->core_id);
        logger.write(LogLevel::error, LogCategory::core, message);
        throw std::runtime_error(message);
    }
    std::string contract_error;
    if (!validate_core_contract(*core, contract_error)) {
        const auto message = std::string("core contract violation core=") +
                             std::string(best->core_id) + ": " +
                             contract_error;
        logger.write(LogLevel::error, LogCategory::core, message);
        throw std::runtime_error(message);
    }
    const auto& descriptor = core->descriptor();
    if (descriptor.core_id != best->core_id ||
        descriptor.core_name != best->core_name) {
        const auto message =
            std::string("core descriptor does not match factory: factory_id=") +
            std::string(best->core_id) + " descriptor_id=" +
            std::string(descriptor.core_id) + " factory_name=" +
            std::string(best->core_name) + " descriptor_name=" +
            std::string(descriptor.core_name);
        logger.write(LogLevel::error, LogCategory::core, message);
        throw std::runtime_error(message);
    }
    return core;
}

std::unique_ptr<EmulatorCore> CoreRegistry::create_from_file(
    const std::filesystem::path& path, const CoreLoadOptions& options) const {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open ROM: " + path.string());
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
    if (input.bad()) throw std::runtime_error("Could not read ROM: " + path.string());
    auto resolved = options;
    if (resolved.source_path.empty()) resolved.source_path = path;
    return create(std::move(bytes), resolved);
}

const CoreRegistry& built_in_core_registry() {
    static const CoreRegistry registry{built_in_core_factories()};
    return registry;
}

std::unique_ptr<EmulatorCore> create_core(std::vector<std::uint8_t> rom,
                                          const CoreLoadOptions& options) {
    return built_in_core_registry().create(std::move(rom), options);
}

std::unique_ptr<EmulatorCore> create_core_from_file(
    const std::filesystem::path& path, const CoreLoadOptions& options) {
    return built_in_core_registry().create_from_file(path, options);
}

} // namespace gbb
