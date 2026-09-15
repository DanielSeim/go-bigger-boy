#include "gbb/save_sync.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

gbb::save_sync::Artifact artifact(const gbb::save_sync::ArtifactKind kind,
                                  const std::uint64_t hash,
                                  const std::uint64_t revision = 1) {
    gbb::save_sync::Artifact result;
    result.system_id = "gbc";
    result.rom_fingerprint = UINT64_C(0x0123456789ABCDEF);
    result.kind = kind;
    result.slot = kind == gbb::save_sync::ArtifactKind::save_state ? 2 : 0;
    result.byte_size = 4;
    result.content_hash = hash;
    result.revision = revision;
    result.base_revision = revision - 1;
    result.base_content_hash = revision == 1 ? 0 : UINT64_C(0x1111);
    result.updated_at = 1700000000;
    result.device_id = "test-device";
    result.core_id = "gbb.gameboy";
    result.hardware_model = "cgb";
    return result;
}

void test_hash() {
    const std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03, 0x04};
    check(gbb::save_sync::hash_bytes(bytes.data(), bytes.size()) ==
              UINT64_C(0xBE7A5E775165785D),
          "content hash is stable");
    check(gbb::save_sync::hash_bytes(nullptr, 0) != 0,
          "empty content has a non-zero hash");
    check(gbb::save_sync::hash_bytes(nullptr, 1) == 0,
          "invalid null content is rejected by the hash helper");
}

void test_validation() {
    const auto save = artifact(gbb::save_sync::ArtifactKind::battery_save,
                               UINT64_C(0xAAAA));
    std::string error;
    check(gbb::save_sync::is_valid(save, &error),
          "valid battery artifact passes validation");

    auto invalid = save;
    invalid.revision = 0;
    check(!gbb::save_sync::is_valid(invalid, &error) &&
              error == "artifact revision must be non-zero",
          "zero revisions are rejected");

    gbb::save_sync::Manifest manifest{"test-device", {save}};
    check(gbb::save_sync::is_valid(manifest, &error),
          "valid manifest passes validation");
    manifest.artifacts.front().device_id = "another-device";
    check(gbb::save_sync::is_valid(manifest, &error),
          "manifest may contain artifacts from another device");
    manifest.artifacts.push_back(save);
    check(!gbb::save_sync::is_valid(manifest, &error) &&
              error == "manifest contains duplicate artifacts",
          "duplicate artifacts are rejected");
}

void test_reconciliation() {
    const auto local = artifact(gbb::save_sync::ArtifactKind::battery_save,
                                UINT64_C(0xAAAA));
    auto same = local;
    check(gbb::save_sync::reconcile(local, same) ==
              gbb::save_sync::Reconciliation::identical,
          "matching content is identical");

    auto local_child = artifact(gbb::save_sync::ArtifactKind::battery_save,
                                UINT64_C(0xBBBB), 2);
    local_child.base_content_hash = local.content_hash;
    check(gbb::save_sync::reconcile(local_child, local) ==
              gbb::save_sync::Reconciliation::local_newer,
          "a known local child is newer");
    check(gbb::save_sync::reconcile(local, local_child) ==
              gbb::save_sync::Reconciliation::remote_newer,
          "a known remote child is newer");

    auto divergent = local_child;
    divergent.content_hash = UINT64_C(0xCCCC);
    divergent.base_content_hash = local.content_hash;
    check(gbb::save_sync::reconcile(divergent, local_child) ==
              gbb::save_sync::Reconciliation::conflict,
          "divergent records at one revision conflict");

    auto different_rom = local;
    ++different_rom.rom_fingerprint;
    check(gbb::save_sync::reconcile(local, different_rom) ==
              gbb::save_sync::Reconciliation::conflict,
          "different ROM identities conflict");
}

void test_json_schema() {
    const auto battery = artifact(gbb::save_sync::ArtifactKind::battery_save,
                                  UINT64_C(0xAAAA));
    const auto state = artifact(gbb::save_sync::ArtifactKind::save_state,
                                UINT64_C(0xBBBB));
    const gbb::save_sync::Manifest manifest{"test-device", {battery, state}};
    const auto json = gbb::save_sync::to_json(manifest);
    check(json.find("\"schema\":\"gbb.save-sync.v1\"") != std::string::npos,
          "manifest declares the stable schema name");
    check(json.find("\"manifest_version\":1") != std::string::npos,
          "manifest declares its version");
    check(json.find("\"rom_fingerprint\":\"0x123456789abcdef\"") !=
              std::string::npos,
          "64-bit fingerprints are JSON strings");
    check(json.find("\"kind\":\"battery-save\"") != std::string::npos &&
              json.find("\"kind\":\"save-state\"") != std::string::npos,
          "manifest names each artifact kind");
    check(json.find("\"slot\":2") != std::string::npos,
          "manifest carries save-state slots");
    check(json.back() == '\n', "manifest has a newline for stable file output");
    check(gbb::save_sync::artifact_key(battery) ==
              "0x123456789abcdef/battery-save/0",
          "artifact keys are stable and provider-neutral");
}

} // namespace

int main() {
    test_hash();
    test_validation();
    test_reconciliation();
    test_json_schema();
    return failures == 0 ? 0 : 1;
}
