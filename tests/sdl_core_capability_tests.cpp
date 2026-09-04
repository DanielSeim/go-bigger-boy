#include "core_capability.hpp"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

class CapabilityCore final : public gbb::EmulatorCore {
public:
    const gbb::CoreDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    void reset() noexcept override {}
    unsigned step_instruction() override { return 4; }
    bool frame_ready() const noexcept override { return false; }
    void consume_frame() noexcept override {}
    gbb::VideoFrameView video_frame() const noexcept override { return {}; }
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
        gbb::PersistentDataKind,
        const std::vector<std::uint8_t>&) override {}

    gbb::CoreCapability& capabilities() noexcept {
        return descriptor_.capabilities;
    }

private:
    gbb::CoreDescriptor descriptor_{
        "test", "Capability test core", gbb::SystemId::game_boy,
        160, 144, 60.0, 4194304.0, 70224, 44100, 2, nullptr, 0};
};

} // namespace

int main() {
    const auto tools = gbb::CoreCapability::debugger |
                       gbb::CoreCapability::sprite_editor;
    if (!gbb::sdl::supports(tools, gbb::CoreCapability::debugger) ||
        !gbb::sdl::supports(tools, gbb::CoreCapability::sprite_editor) ||
        gbb::sdl::supports(tools, gbb::CoreCapability::link_cable) ||
        gbb::sdl::supports(nullptr, gbb::CoreCapability::debugger)) {
        std::cerr << "core capability gating regression\n";
        return 1;
    }
    const gbb::sdl::CoreServices adapter{nullptr, nullptr};
    if (adapter.get(gbb::CoreCapability::debugger) != nullptr ||
        adapter.debugger() != nullptr || adapter.sprite_editor() != nullptr ||
        adapter.cheats() != nullptr || adapter.link_cable() != nullptr) {
        std::cerr << "null core must not expose Game Boy tools\n";
        return 1;
    }

    CapabilityCore core;
    auto* const sentinel = reinterpret_cast<gameboy::Emulator*>(uintptr_t{1});
    gbb::sdl::CoreServices services{&core, sentinel};
    if (services.debugger() != nullptr || services.link_cable() != nullptr) {
        std::cerr << "services must reject unsupported capabilities\n";
        return 1;
    }
    core.capabilities() = gbb::CoreCapability::debugger |
                          gbb::CoreCapability::link_cable;
    if (services.debugger() != sentinel || services.link_cable() != sentinel ||
        services.sprite_editor() != nullptr) {
        std::cerr << "services must expose only advertised capabilities\n";
        return 1;
    }
    services = {nullptr, sentinel};
    if (services.debugger() != nullptr || services.link_cable() != nullptr) {
        std::cerr << "services must be refreshed when the core closes\n";
        return 1;
    }
    return 0;
}
