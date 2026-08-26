#include "gbb/core_registry.hpp"
#include "gbb/gameboy_core.hpp"

#include "gameboy/cartridge.hpp"
#include "gameboy/emulator.hpp"

#include <array>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gbb {
namespace {

CoreProbeResult probe_game_boy(
    const std::vector<std::uint8_t>& rom,
    const CoreLoadOptions&) noexcept {
    if (rom.size() < 0x150) return {};
    const auto cgb_flag = rom[0x143];
    return {50, (cgb_flag & 0x80) != 0 ? SystemId::game_boy_color
                                       : SystemId::game_boy};
}

constexpr std::array<InputDescriptor, 8> game_boy_inputs{{
    {InputId::right, "Right"}, {InputId::left, "Left"},
    {InputId::up, "Up"},       {InputId::down, "Down"},
    {InputId::a, "A"},         {InputId::b, "B"},
    {InputId::select, "Select"}, {InputId::start, "Start"},
}};

gameboy::Button game_boy_button(const InputId input) noexcept {
    switch (input) {
    case InputId::right: return gameboy::Button::right;
    case InputId::left: return gameboy::Button::left;
    case InputId::up: return gameboy::Button::up;
    case InputId::down: return gameboy::Button::down;
    case InputId::a: return gameboy::Button::a;
    case InputId::b: return gameboy::Button::b;
    case InputId::select: return gameboy::Button::select;
    case InputId::start: return gameboy::Button::start;
    default: return gameboy::Button::a;
    }
}

bool is_game_boy_input(const InputId input) noexcept {
    return input == InputId::right || input == InputId::left ||
           input == InputId::up || input == InputId::down ||
           input == InputId::a || input == InputId::b ||
           input == InputId::select || input == InputId::start;
}

class GameBoyCore final : public EmulatorCore {
public:
    explicit GameBoyCore(gameboy::Cartridge cartridge)
        : emulator_(std::move(cartridge)) {
        descriptor_.system = emulator_.bus().cgb_mode()
                                 ? SystemId::game_boy_color
                                 : SystemId::game_boy;
        descriptor_.capabilities =
            CoreCapability::compatibility_palette |
            CoreCapability::cheats | CoreCapability::debugger |
            CoreCapability::sprite_editor | CoreCapability::printer;
        if (emulator_.has_battery()) {
            descriptor_.capabilities = descriptor_.capabilities |
                                       CoreCapability::persistent_memory;
        }
        if (emulator_.has_rtc()) {
            descriptor_.capabilities = descriptor_.capabilities |
                                       CoreCapability::rtc;
        }
        if (emulator_.has_rumble()) {
            descriptor_.capabilities = descriptor_.capabilities |
                                       CoreCapability::rumble;
        }
        if (emulator_.has_camera()) {
            descriptor_.capabilities = descriptor_.capabilities |
                                       CoreCapability::camera;
        }
    }

    const CoreDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    void reset() noexcept override { emulator_.reset(); }
    unsigned step_instruction() override { return emulator_.step(); }
    bool frame_ready() const noexcept override { return emulator_.frame_ready(); }
    void consume_frame() noexcept override { emulator_.consume_frame(); }
    VideoFrameView video_frame() const noexcept override {
        const auto& frame = emulator_.framebuffer();
        return {frame.data(), frame.size(), gameboy::Ppu::screen_width,
                gameboy::Ppu::screen_height,
                gameboy::Ppu::screen_width * sizeof(std::uint32_t)};
    }
    std::vector<std::int16_t> take_audio_samples() override {
        return emulator_.take_audio_samples();
    }
    void set_input(const InputId input, const bool pressed) noexcept override {
        if (is_game_boy_input(input)) {
            emulator_.set_button(game_boy_button(input), pressed);
        }
    }
    std::vector<std::uint8_t> save_state() const override {
        return emulator_.save_state();
    }
    void load_state(const std::vector<std::uint8_t>& state) override {
        emulator_.load_state(state);
    }
    std::uint64_t rom_fingerprint() const noexcept override {
        return emulator_.rom_fingerprint();
    }
    void flush_persistent_data() override { emulator_.flush_battery(); }
    bool has_persistent_data(const PersistentDataKind kind) const noexcept override {
        if (kind == PersistentDataKind::rtc) return emulator_.has_rtc();
        return emulator_.has_battery();
    }
    std::vector<std::uint8_t> export_persistent_data(
        const PersistentDataKind kind) const override {
        switch (kind) {
        case PersistentDataKind::battery_ram:
            return emulator_.export_battery_ram();
        case PersistentDataKind::battery_save:
            return emulator_.export_battery_save();
        case PersistentDataKind::rtc:
            return emulator_.export_rtc_data();
        }
        return {};
    }
    void import_persistent_data(
        const PersistentDataKind kind,
        const std::vector<std::uint8_t>& data) override {
        switch (kind) {
        case PersistentDataKind::battery_ram:
            emulator_.import_battery_ram(data);
            break;
        case PersistentDataKind::battery_save:
            emulator_.import_battery_save(data);
            break;
        case PersistentDataKind::rtc:
            emulator_.import_rtc_data(data);
            break;
        }
    }
    bool rumble_active() const noexcept override {
        return emulator_.rumble_active();
    }
    void set_camera_frame(const std::uint8_t* pixels,
                          const std::size_t size) noexcept override {
        emulator_.set_camera_frame(pixels, size);
    }
    void set_compatibility_colors(const bool enabled) noexcept override {
        emulator_.set_dmg_compatibility_colors(enabled);
    }

    gameboy::Emulator& emulator() noexcept { return emulator_; }
    const gameboy::Emulator& emulator() const noexcept { return emulator_; }

private:
    gameboy::Emulator emulator_;
    CoreDescriptor descriptor_{
        "gb", "Game Boy / Game Boy Color", SystemId::game_boy,
        gameboy::Ppu::screen_width, gameboy::Ppu::screen_height,
        4194304.0 / 70224.0, 4194304.0, 70224,
        gameboy::Apu::sample_rate, 2,
        game_boy_inputs.data(), game_boy_inputs.size(), CoreCapability::none,
    };
};

} // namespace

CoreRegistry::CoreRegistry(std::vector<CoreFactory> factories)
    : factories_(std::move(factories)) {}

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
    for (const auto& factory : factories_) {
        const auto candidate = factory.probe(rom, options);
        if (candidate.confidence > confidence) {
            confidence = candidate.confidence;
            best = &factory;
        }
    }
    if (best == nullptr) {
        throw std::runtime_error("No installed emulator core recognizes this ROM");
    }
    return best->create(std::move(rom), options);
}

const CoreRegistry& built_in_core_registry() {
    static const CoreRegistry registry{{CoreFactory{
        "gb", "Game Boy / Game Boy Color", probe_game_boy,
        [](std::vector<std::uint8_t> rom,
           const CoreLoadOptions& options) -> std::unique_ptr<EmulatorCore> {
            gameboy::Cartridge cartridge(std::move(rom));
            if (!options.persistence_path.empty()) {
                cartridge.set_persistence_path(options.persistence_path);
            } else if (!options.source_path.empty()) {
                cartridge.set_persistence_path(options.source_path);
            }
            return std::make_unique<GameBoyCore>(std::move(cartridge));
        }}}};
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

gameboy::Emulator* gameboy_emulator(EmulatorCore* core) noexcept {
    const auto adapter = dynamic_cast<GameBoyCore*>(core);
    return adapter == nullptr ? nullptr : &adapter->emulator();
}

const gameboy::Emulator* gameboy_emulator(const EmulatorCore* core) noexcept {
    const auto adapter = dynamic_cast<const GameBoyCore*>(core);
    return adapter == nullptr ? nullptr : &adapter->emulator();
}

} // namespace gbb
