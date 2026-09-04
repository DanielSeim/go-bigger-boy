#include "gbb/core_contract.hpp"
#include "gbb/core_registry.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> test_rom() {
    std::vector<std::uint8_t> rom(0x8000, 0);
    constexpr std::string_view title = "CORE CONTRACT";
    std::copy(title.begin(), title.end(), rom.begin() + 0x134);
    return rom;
}

class ContractTestCore final : public gbb::EmulatorCore {
public:
    ContractTestCore(const std::size_t width, const std::size_t height,
                     const std::uint32_t* pixels, const std::size_t pixel_count,
                     const std::size_t pitch)
        : frame_{pixels, pixel_count, width, height, pitch} {}

    const gbb::CoreDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    void reset() noexcept override {}
    unsigned step_instruction() override { return 4; }
    bool frame_ready() const noexcept override { return false; }
    void consume_frame() noexcept override {}
    gbb::VideoFrameView video_frame() const noexcept override { return frame_; }
    const gbb::SceneSnapshot& scene_snapshot() const noexcept override {
        return scene_;
    }
    std::vector<std::int16_t> take_audio_samples() override { return {}; }
    void set_input(gbb::InputId, bool) noexcept override {}
    std::vector<std::uint8_t> save_state() const override { return {}; }
    void load_state(const std::vector<std::uint8_t>&) override {}
    std::uint64_t rom_fingerprint() const noexcept override { return 0; }
    void flush_persistent_data() override {}
    bool has_persistent_data(gbb::PersistentDataKind) const noexcept override {
        return false;
    }
    std::vector<std::uint8_t> export_persistent_data(
        gbb::PersistentDataKind) const override {
        return {};
    }
    void import_persistent_data(
        gbb::PersistentDataKind, const std::vector<std::uint8_t>&) override {}

    gbb::CoreDescriptor& mutable_descriptor() noexcept { return descriptor_; }
    gbb::SceneSnapshot& mutable_scene() noexcept { return scene_; }

private:
    gbb::CoreDescriptor descriptor_{
        "test", "Contract test core", gbb::SystemId::game_boy,
        160, 144, 60.0, 4194304.0, 70224, 44100, 2, nullptr, 0};
    gbb::VideoFrameView frame_{};
    gbb::SceneSnapshot scene_{};
};

void test_invalid_core_contracts() {
    static const std::uint32_t pixels[160 * 144]{};
    {
        ContractTestCore core(160, 144, pixels, 160 * 144,
                              160 * sizeof(std::uint32_t));
        std::string error;
        check(gbb::validate_core_contract(core, error),
              "valid test adapter satisfies the core contract");
    }
    {
        ContractTestCore core(160, 144, nullptr, 160 * 144,
                              160 * sizeof(std::uint32_t));
        std::string error;
        check(!gbb::validate_core_contract(core, error) &&
                  error.find("enough pixels") != std::string::npos,
              "contract rejects a missing framebuffer");
    }
    {
        ContractTestCore core(160, 144, pixels, 160 * 144,
                              159 * sizeof(std::uint32_t));
        std::string error;
        check(!gbb::validate_core_contract(core, error) &&
                  error.find("pitch") != std::string::npos,
              "contract rejects a framebuffer with a short pitch");
    }
    {
        ContractTestCore core(160, 144, pixels, 160 * 144,
                              160 * sizeof(std::uint32_t));
        auto& descriptor = core.mutable_descriptor();
        descriptor.input_count = 1;
        descriptor.inputs = nullptr;
        std::string error;
        check(!gbb::validate_core_contract(core, error) &&
                  error.find("input descriptors") != std::string::npos,
              "contract rejects an input count without descriptors");
    }
    {
        ContractTestCore core(160, 144, pixels, 160 * 144,
                              160 * sizeof(std::uint32_t));
        auto& descriptor = core.mutable_descriptor();
        descriptor.capabilities = gbb::CoreCapability::scene_layers;
        core.mutable_scene().width = 159;
        core.mutable_scene().height = 144;
        std::string error;
        check(!gbb::validate_core_contract(core, error) &&
                  error.find("scene dimensions") != std::string::npos,
              "contract rejects scene dimensions that disagree with video");
    }
    {
        ContractTestCore core(160, 144, pixels, 160 * 144,
                              160 * sizeof(std::uint32_t));
        auto& descriptor = core.mutable_descriptor();
        descriptor.capabilities = gbb::CoreCapability::scene_layers;
        auto& scene = core.mutable_scene();
        scene.width = 160;
        scene.height = 144;
        scene.layers.push_back({"", "test.layer.v1", 1, 1, {0x01}});
        std::string error;
        check(!gbb::validate_core_contract(core, error) &&
                  error.find("id and format") != std::string::npos,
              "contract rejects unnamed opaque scene layers");
    }
    {
        ContractTestCore core(160, 144, pixels, 160 * 144,
                              160 * sizeof(std::uint32_t));
        auto& descriptor = core.mutable_descriptor();
        descriptor.capabilities = gbb::CoreCapability::scene_layers;
        auto& scene = core.mutable_scene();
        scene.width = 160;
        scene.height = 144;
        scene.layers.push_back({"test.layer", "test.layer.v1", 1, 1, {}});
        std::string error;
        check(!gbb::validate_core_contract(core, error) &&
                  error.find("payload is empty") != std::string::npos,
              "contract rejects a sized scene layer without payload");
    }
    {
        ContractTestCore core(160, 144, pixels, 160 * 144,
                              160 * sizeof(std::uint32_t));
        auto& descriptor = core.mutable_descriptor();
        descriptor.capabilities = gbb::CoreCapability::scene_layers;
        auto& scene = core.mutable_scene();
        scene.width = 160;
        scene.height = 144;
        scene.layers.push_back({"test.layer", "test.layer.v1", 1, 1, {0x01}});
        scene.layers.push_back({"test.layer", "test.layer.v1", 1, 1, {0x02}});
        std::string error;
        check(!gbb::validate_core_contract(core, error) &&
                  error.find("duplicate id") != std::string::npos,
              "contract rejects duplicate scene layer ids");
    }
    {
        gbb::CoreRegistry registry;
        registry.register_factory({
            "invalid", "Invalid contract core",
            [](const std::vector<std::uint8_t>&,
               const gbb::CoreLoadOptions&) noexcept {
                return gbb::CoreProbeResult{100, gbb::SystemId::game_boy};
            },
            [](std::vector<std::uint8_t>, const gbb::CoreLoadOptions&)
                -> std::unique_ptr<gbb::EmulatorCore> {
                return std::make_unique<ContractTestCore>(
                    160, 144, nullptr, 160 * 144,
                    160 * sizeof(std::uint32_t));
            }});
        bool rejected = false;
        try {
            static_cast<void>(registry.create(std::vector<std::uint8_t>(1)));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected,
              "registry enforces the contract before returning an adapter");
    }
}

} // namespace

int main() {
    test_invalid_core_contracts();
    const auto rom = test_rom();
    const auto& registry = gbb::built_in_core_registry();
    check(!registry.factories().empty(),
          "at least one built-in core is available for contract validation");
    for (const auto& factory : registry.factories()) {
        const auto probe = factory.probe(rom, {});
        check(probe.confidence > 0, "built-in core accepts the contract ROM");
        auto core = factory.create(rom, {});
        check(core != nullptr, "built-in core factory creates an instance");
        if (!core) continue;
        std::string error;
        check(gbb::validate_core_contract(*core, error),
              "built-in core satisfies the frontend contract");
        if (!error.empty()) {
            std::cerr << "contract error for " << factory.core_id << ": "
                      << error << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
