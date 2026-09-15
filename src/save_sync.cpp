#include "gbb/save_sync.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>

namespace gbb::save_sync {
namespace {

constexpr std::uint64_t fnv_offset = UINT64_C(14695981039346656037);
constexpr std::uint64_t fnv_prime = UINT64_C(1099511628211);

const char* kind_name(const ArtifactKind kind) noexcept {
    switch (kind) {
    case ArtifactKind::battery_save: return "battery-save";
    case ArtifactKind::rtc: return "rtc";
    case ArtifactKind::save_state: return "save-state";
    }
    return "unknown";
}

bool known_kind(const ArtifactKind kind) noexcept {
    return kind == ArtifactKind::battery_save ||
           kind == ArtifactKind::rtc || kind == ArtifactKind::save_state;
}

std::string hex_u64(const std::uint64_t value) {
    char digits[16]{};
    const auto result = std::to_chars(std::begin(digits), std::end(digits),
                                      value, 16);
    return "0x" + std::string(digits, result.ptr);
}

class JsonWriter {
public:
    void begin_object() { output_ += '{'; }
    void end_object() {
        trim_comma();
        output_ += '}';
    }
    void begin_array() { output_ += '['; }
    void end_array() {
        trim_comma();
        output_ += ']';
    }
    void key(const std::string_view value) {
        quoted(value);
        output_ += ':';
    }
    void string(const std::string_view value) {
        quoted(value);
        output_ += ',';
    }
    void number(const std::uint64_t value) {
        output_ += std::to_string(value);
        output_ += ',';
    }
    void hex_number(const std::uint64_t value) { string(hex_u64(value)); }
    void signed_number(const std::int64_t value) {
        output_ += std::to_string(value);
        output_ += ',';
    }
    void comma() { output_ += ','; }
    [[nodiscard]] std::string finish() && {
        output_ += '\n';
        return std::move(output_);
    }

private:
    void quoted(const std::string_view value) {
        output_ += '"';
        for (const char character : value) {
            switch (character) {
            case '"': output_ += "\\\""; break;
            case '\\': output_ += "\\\\"; break;
            case '\n': output_ += "\\n"; break;
            case '\r': output_ += "\\r"; break;
            case '\t': output_ += "\\t"; break;
            default: output_ += character; break;
            }
        }
        output_ += '"';
    }
    void trim_comma() {
        if (!output_.empty() && output_.back() == ',') output_.pop_back();
    }

    std::string output_;
};

void write_artifact(JsonWriter& json, const Artifact& artifact) {
    json.begin_object();
    json.key("system_id"); json.string(artifact.system_id);
    json.key("rom_fingerprint"); json.hex_number(artifact.rom_fingerprint);
    json.key("kind"); json.string(kind_name(artifact.kind));
    json.key("slot"); json.number(artifact.slot);
    json.key("byte_size"); json.number(artifact.byte_size);
    json.key("content_hash"); json.hex_number(artifact.content_hash);
    json.key("revision"); json.hex_number(artifact.revision);
    json.key("base_revision"); json.hex_number(artifact.base_revision);
    json.key("base_content_hash"); json.hex_number(artifact.base_content_hash);
    json.key("updated_at"); json.signed_number(artifact.updated_at);
    json.key("device_id"); json.string(artifact.device_id);
    json.key("core_id"); json.string(artifact.core_id);
    json.key("hardware_model"); json.string(artifact.hardware_model);
    json.end_object();
    json.comma();
}

} // namespace

const char* artifact_kind_name(const ArtifactKind kind) noexcept {
    return kind_name(kind);
}

std::string artifact_key(const Artifact& artifact) {
    return hex_u64(artifact.rom_fingerprint) + "/" + kind_name(artifact.kind) +
           "/" + std::to_string(artifact.slot);
}

bool is_valid(const Artifact& artifact, std::string* error) noexcept {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (artifact.system_id.empty() || artifact.system_id.size() > 32) {
        return fail("artifact system ID is missing or too long");
    }
    if (artifact.rom_fingerprint == 0) {
        return fail("artifact ROM fingerprint is missing");
    }
    if (!known_kind(artifact.kind)) {
        return fail("artifact kind is unknown");
    }
    if (artifact.kind == ArtifactKind::save_state && artifact.slot > 99) {
        return fail("save-state slot is out of range");
    }
    if (artifact.kind != ArtifactKind::save_state && artifact.slot != 0) {
        return fail("battery and RTC artifacts must use slot zero");
    }
    if (artifact.byte_size > maximum_artifact_size) {
        return fail("artifact is too large");
    }
    if (artifact.revision == 0) {
        return fail("artifact revision must be non-zero");
    }
    if (artifact.base_revision >= artifact.revision) {
        return fail("artifact base revision must precede its revision");
    }
    if (artifact.updated_at < 0) {
        return fail("artifact update time cannot be negative");
    }
    if (artifact.device_id.empty() || artifact.device_id.size() > 128) {
        return fail("artifact device ID is missing or too long");
    }
    if (artifact.core_id.size() > 64 || artifact.hardware_model.size() > 32) {
        return fail("artifact metadata is too long");
    }
    return true;
}

bool is_valid(const Manifest& manifest, std::string* error) noexcept {
    const auto fail = [error](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (manifest.device_id.empty() || manifest.device_id.size() > 128) {
        return fail("manifest device ID is missing or too long");
    }
    for (std::size_t index = 0; index < manifest.artifacts.size(); ++index) {
        std::string artifact_error;
        if (!is_valid(manifest.artifacts[index], &artifact_error)) {
            if (error) *error = "artifact " + std::to_string(index) + ": " +
                                artifact_error;
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto& left = manifest.artifacts[previous];
            const auto& right = manifest.artifacts[index];
            if (left.rom_fingerprint == right.rom_fingerprint &&
                left.kind == right.kind && left.slot == right.slot) {
                return fail("manifest contains duplicate artifacts");
            }
        }
    }
    return true;
}

Reconciliation reconcile(const Artifact& local,
                         const Artifact& remote) noexcept {
    if (local.system_id != remote.system_id ||
        local.rom_fingerprint != remote.rom_fingerprint ||
        local.kind != remote.kind || local.slot != remote.slot) {
        return Reconciliation::conflict;
    }
    if (local.byte_size == remote.byte_size &&
        local.content_hash == remote.content_hash) {
        return Reconciliation::identical;
    }
    if (local.revision > remote.revision &&
        local.base_revision == remote.revision &&
        local.base_content_hash == remote.content_hash) {
        return Reconciliation::local_newer;
    }
    if (remote.revision > local.revision &&
        remote.base_revision == local.revision &&
        remote.base_content_hash == local.content_hash) {
        return Reconciliation::remote_newer;
    }
    return Reconciliation::conflict;
}

std::uint64_t hash_bytes(const std::uint8_t* bytes,
                         const std::size_t size) noexcept {
    if (!bytes && size != 0) return 0;
    auto hash = fnv_offset;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= fnv_prime;
    }
    return hash;
}

std::string to_json(const Manifest& manifest) {
    JsonWriter json;
    json.begin_object();
    json.key("schema"); json.string("gbb.save-sync.v1");
    json.key("manifest_version"); json.number(manifest_version);
    json.key("device_id"); json.string(manifest.device_id);
    json.key("artifacts");
    json.begin_array();
    for (const auto& artifact : manifest.artifacts) write_artifact(json, artifact);
    json.end_array();
    json.end_object();
    return std::move(json).finish();
}

bool write_json(const Manifest& manifest,
                const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::string error;
    if (!is_valid(manifest, &error)) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output << to_json(manifest);
    return static_cast<bool>(output);
}

} // namespace gbb::save_sync
