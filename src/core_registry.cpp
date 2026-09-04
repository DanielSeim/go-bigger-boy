#include "gbb/core_registry.hpp"
#include "gbb/core_contract.hpp"
#include "gbb/core_contributors.hpp"
#include "gbb/log.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>

namespace gbb {

CoreRegistry::CoreRegistry(std::vector<CoreFactory> factories)
{
    factories_.reserve(factories.size());
    for (const auto& factory : factories) register_factory(factory);
}

void CoreRegistry::register_factory(CoreFactory factory) {
    if (factory.core_id.empty() || factory.probe == nullptr ||
        factory.create == nullptr) {
        throw std::invalid_argument("A core factory requires an id, probe, and creator");
    }
    const auto duplicate = std::find_if(
        factories_.begin(), factories_.end(), [&](const CoreFactory& existing) {
            return existing.core_id == factory.core_id;
        });
    if (duplicate != factories_.end()) {
        throw std::invalid_argument("Duplicate core id: " +
                                    std::string(factory.core_id));
    }
    factories_.push_back(factory);
}

const std::vector<CoreFactory>& CoreRegistry::factories() const noexcept {
    return factories_;
}

std::vector<CoreProbeMatch> CoreRegistry::probe_matches(
    const std::vector<std::uint8_t>& rom,
    const CoreLoadOptions& options) const {
    std::vector<CoreProbeMatch> matches;
    matches.reserve(factories_.size());
    for (const auto& factory : factories_) {
        matches.push_back({factory.core_id, factory.probe(rom, options)});
    }
    return matches;
}

CoreProbeResult CoreRegistry::probe(
    const std::vector<std::uint8_t>& rom,
    const CoreLoadOptions& options) const noexcept {
    CoreProbeResult best{};
    for (const auto& factory : factories_) {
        const auto candidate = factory.probe(rom, options);
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
    auto core = best->create(std::move(rom), options);
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
    return core;
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
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open ROM: " + path.string());
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
    if (input.bad()) throw std::runtime_error("Could not read ROM: " + path.string());
    auto resolved = options;
    if (resolved.source_path.empty()) resolved.source_path = path;
    return create_core(std::move(bytes), resolved);
}

} // namespace gbb
