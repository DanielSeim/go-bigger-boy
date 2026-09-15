#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace gbb::save_sync {

// This is metadata for a future provider; the save bytes remain separate.
// Keeping the manifest independent of a provider lets Android, desktop, and
// web use the same conflict rules without sharing authentication code.
constexpr std::uint32_t manifest_version = 1;
constexpr std::uint64_t maximum_artifact_size = 32 * 1024 * 1024;

enum class ArtifactKind {
    battery_save,
    rtc,
    save_state,
};

struct Artifact {
    std::string system_id;       // Stable ID such as "gb" or "gbc".
    std::uint64_t rom_fingerprint{};
    ArtifactKind kind = ArtifactKind::battery_save;
    std::uint32_t slot{};        // Battery/RTC use slot zero.
    std::uint64_t byte_size{};
    std::uint64_t content_hash{};
    std::uint64_t revision{};
    std::uint64_t base_revision{};
    std::uint64_t base_content_hash{};
    std::int64_t updated_at{};   // Unix seconds, supplied by the frontend.
    std::string device_id;
    std::string core_id;
    std::string hardware_model;
};

struct Manifest {
    std::string device_id;
    std::vector<Artifact> artifacts;
};

enum class Reconciliation {
    identical,
    local_newer,
    remote_newer,
    conflict,
};

[[nodiscard]] const char* artifact_kind_name(ArtifactKind kind) noexcept;
[[nodiscard]] std::string artifact_key(const Artifact& artifact);
[[nodiscard]] bool is_valid(const Artifact& artifact,
                            std::string* error = nullptr) noexcept;
[[nodiscard]] bool is_valid(const Manifest& manifest,
                            std::string* error = nullptr) noexcept;

// A revision only wins automatically when the other record is its known
// parent. Divergent records from the same parent are deliberately conflicts.
[[nodiscard]] Reconciliation reconcile(const Artifact& local,
                                       const Artifact& remote) noexcept;

[[nodiscard]] std::uint64_t hash_bytes(const std::uint8_t* bytes,
                                       std::size_t size) noexcept;

// Canonical, provider-neutral JSON for the manifest. Save payloads are not
// embedded; a provider stores them as separate objects keyed by artifact_key.
[[nodiscard]] std::string to_json(const Manifest& manifest);
[[nodiscard]] bool write_json(const Manifest& manifest,
                              const std::filesystem::path& path);

} // namespace gbb::save_sync
