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
            CoreCapability::sprite_editor | CoreCapability::printer |
            CoreCapability::scene_layers | CoreCapability::link_cable;
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
        scene_.width = gameboy::Ppu::screen_width;
        scene_.height = gameboy::Ppu::screen_height;
        scene_.background.width = 32;
        scene_.background.height = 32;
        scene_.background.tile_ids.resize(32 * 32);
        scene_.background.attributes.resize(32 * 32);
        scene_.window.width = 32;
        scene_.window.height = 32;
        scene_.window.tile_ids.resize(32 * 32);
        scene_.window.attributes.resize(32 * 32);
        scene_.tile_size_bytes = 16;
        scene_.tile_count = 384;
        scene_.tile_banks = bus_cgb_mode() ? 2 : 1;
        scene_.tile_bank_stride = scene_.tile_count * scene_.tile_size_bytes;
        scene_.tile_data.resize(scene_.tile_banks * scene_.tile_bank_stride);
        scene_.cgb_bg_palette.resize(0x40);
        scene_.cgb_object_palette.resize(0x40);
        scene_.sprites.resize(40);
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
    const SceneSnapshot& scene_snapshot() const noexcept override {
        refresh_scene_snapshot();
        return scene_;
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
    [[nodiscard]] bool bus_cgb_mode() const noexcept {
        return emulator_.bus().cgb_mode();
    }

    void refresh_scene_snapshot() const noexcept {
        const auto& bus = emulator_.bus();
        auto& scene = scene_;
        scene.emulation_cycles = emulator_.cpu().total_cycles();
        scene.width = gameboy::Ppu::screen_width;
        scene.height = gameboy::Ppu::screen_height;
        scene.cgb_mode = bus.cgb_mode();
        scene.lcdc = bus.read8(0xFF40);
        scene.scx = bus.read8(0xFF43);
        scene.scy = bus.read8(0xFF42);
        scene.wx = bus.read8(0xFF4B);
        scene.wy = bus.read8(0xFF4A);
        scene.bg_palette = bus.read8(0xFF47);
        scene.object_palette_0 = bus.read8(0xFF48);
        scene.object_palette_1 = bus.read8(0xFF49);
        scene.bg_palette_index = bus.read8(0xFF68);
        scene.object_palette_index = bus.read8(0xFF6A);

        const auto fill_layer = [&](SceneTileLayer& layer,
                                    const std::uint16_t address,
                                    const bool enabled) {
            layer.enabled = enabled;
            layer.map_address = address;
            layer.width = 32;
            layer.height = 32;
            layer.tile_data_unsigned = (scene.lcdc & 0x10U) != 0;
            layer.tile_ids.resize(layer.width * layer.height);
            layer.attributes.resize(layer.width * layer.height);
            const auto offset = static_cast<std::uint16_t>(address - 0x8000);
            for (std::size_t index = 0; index < layer.tile_ids.size(); ++index) {
                const auto map_offset = static_cast<std::uint16_t>(
                    offset + static_cast<std::uint16_t>(index));
                layer.tile_ids[index] = bus.debug_read_vram(0, map_offset);
                layer.attributes[index] = scene.cgb_mode
                                              ? bus.debug_read_vram(1, map_offset)
                                              : 0;
            }
        };
        const auto bg_map = (scene.lcdc & 0x08U) != 0 ? 0x9C00 : 0x9800;
        const auto window_map = (scene.lcdc & 0x40U) != 0 ? 0x9C00 : 0x9800;
        fill_layer(scene.background, bg_map, (scene.lcdc & 0x01U) != 0);
        fill_layer(scene.window, window_map, (scene.lcdc & 0x20U) != 0);

        scene.tile_size_bytes = 16;
        scene.tile_count = 384;
        scene.tile_banks = scene.cgb_mode ? 2 : 1;
        scene.tile_bank_stride = scene.tile_count * scene.tile_size_bytes;
        scene.tile_data.resize(scene.tile_banks * scene.tile_bank_stride);
        for (std::size_t bank = 0; bank < scene.tile_banks; ++bank) {
            for (std::size_t byte = 0; byte < scene.tile_bank_stride; ++byte) {
                scene.tile_data[bank * scene.tile_bank_stride + byte] =
                    bus.debug_read_vram(
                        static_cast<std::uint8_t>(bank),
                        static_cast<std::uint16_t>(byte));
            }
        }
        scene.cgb_bg_palette.resize(0x40);
        scene.cgb_object_palette.resize(0x40);
        for (std::size_t index = 0; index < scene.cgb_bg_palette.size(); ++index) {
            scene.cgb_bg_palette[index] = bus.debug_read_cgb_bg_palette(
                static_cast<std::uint8_t>(index));
            scene.cgb_object_palette[index] = bus.debug_read_cgb_object_palette(
                static_cast<std::uint8_t>(index));
        }
        scene.sprites.resize(40);
        const auto object_height = (scene.lcdc & 0x04U) != 0 ? 16 : 8;
        for (std::size_t index = 0; index < scene.sprites.size(); ++index) {
            const auto offset = static_cast<std::uint8_t>(index * 4);
            auto& sprite = scene.sprites[index];
            sprite.oam_y = bus.debug_read_oam(offset);
            sprite.oam_x = bus.debug_read_oam(static_cast<std::uint8_t>(offset + 1));
            sprite.tile = bus.debug_read_oam(static_cast<std::uint8_t>(offset + 2));
            sprite.attributes = bus.debug_read_oam(
                static_cast<std::uint8_t>(offset + 3));
            sprite.screen_x = static_cast<std::int16_t>(sprite.oam_x) - 8;
            sprite.screen_y = static_cast<std::int16_t>(sprite.oam_y) - 16;
            sprite.visible = (scene.lcdc & 0x02U) != 0 &&
                             sprite.oam_x != 0 && sprite.oam_y != 0 &&
                             sprite.screen_x < static_cast<std::int16_t>(scene.width) &&
                             sprite.screen_x + 8 > 0 &&
                             sprite.screen_y < static_cast<std::int16_t>(scene.height) &&
                             sprite.screen_y + object_height > 0;
        }
    }

    gameboy::Emulator emulator_;
    mutable SceneSnapshot scene_{};
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
