#pragma once

#include "gbb/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace gbb {

enum class SystemId : std::uint8_t {
    game_boy,
    game_boy_color,
    game_boy_advance,
    unknown = 0xFF,
};

[[nodiscard]] constexpr std::string_view system_id_string(
    const SystemId system) noexcept {
    switch (system) {
    case SystemId::game_boy: return "gb";
    case SystemId::game_boy_color: return "gbc";
    case SystemId::game_boy_advance: return "gba";
    default: return "unknown";
    }
}

[[nodiscard]] constexpr SystemId system_id_from_string(
    const std::string_view value) noexcept {
    return value == "gb" ? SystemId::game_boy
         : value == "gbc" ? SystemId::game_boy_color
         : value == "gba" ? SystemId::game_boy_advance
                           : SystemId::unknown;
}

enum class InputId : std::uint8_t {
    right,
    left,
    up,
    down,
    a,
    b,
    x,
    y,
    l,
    r,
    select,
    start,
};

struct InputDescriptor {
    InputId id{};
    std::string_view name;
};

enum class CoreCapability : std::uint64_t {
    none = 0,
    persistent_memory = UINT64_C(1) << 0,
    rtc = UINT64_C(1) << 1,
    rumble = UINT64_C(1) << 2,
    camera = UINT64_C(1) << 3,
    printer = UINT64_C(1) << 4,
    compatibility_palette = UINT64_C(1) << 5,
    cheats = UINT64_C(1) << 6,
    debugger = UINT64_C(1) << 7,
    sprite_editor = UINT64_C(1) << 8,
    scene_layers = UINT64_C(1) << 9,
};

[[nodiscard]] constexpr CoreCapability operator|(const CoreCapability left,
                                                  const CoreCapability right) {
    return static_cast<CoreCapability>(static_cast<std::uint64_t>(left) |
                                       static_cast<std::uint64_t>(right));
}

[[nodiscard]] constexpr bool has_capability(const CoreCapability set,
                                             const CoreCapability value) {
    return (static_cast<std::uint64_t>(set) &
            static_cast<std::uint64_t>(value)) != 0;
}

enum class PersistentDataKind : std::uint8_t {
    battery_ram,
    battery_save,
    rtc,
};

struct CoreDescriptor {
    std::string_view core_id;
    std::string_view core_name;
    SystemId system{SystemId::unknown};
    std::size_t video_width{};
    std::size_t video_height{};
    double refresh_rate{};
    double clock_rate{};
    unsigned nominal_cycles_per_frame{};
    unsigned audio_sample_rate{};
    unsigned audio_channels{};
    const InputDescriptor* inputs{};
    std::size_t input_count{};
    CoreCapability capabilities{CoreCapability::none};
};

struct VideoFrameView {
    const std::uint32_t* pixels{};
    std::size_t pixel_count{};
    std::size_t width{};
    std::size_t height{};
    std::size_t pitch{};
};

class EmulatorCore {
public:
    virtual ~EmulatorCore() = default;

    [[nodiscard]] virtual const CoreDescriptor& descriptor() const noexcept = 0;
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual unsigned step_instruction() = 0;
    [[nodiscard]] virtual bool frame_ready() const noexcept = 0;
    virtual void consume_frame() noexcept = 0;
    [[nodiscard]] virtual VideoFrameView video_frame() const noexcept = 0;
    // Optional read-only scene data for presentation renderers such as a
    // voxel diorama. The ordinary framebuffer remains the universal fallback.
    [[nodiscard]] virtual const SceneSnapshot& scene_snapshot() const noexcept {
        static const SceneSnapshot empty{};
        return empty;
    }
    [[nodiscard]] virtual std::vector<std::int16_t> take_audio_samples() = 0;
    virtual void set_input(InputId input, bool pressed) noexcept = 0;

    [[nodiscard]] virtual std::vector<std::uint8_t> save_state() const = 0;
    virtual void load_state(const std::vector<std::uint8_t>& state) = 0;
    [[nodiscard]] virtual std::uint64_t rom_fingerprint() const noexcept = 0;

    virtual void flush_persistent_data() = 0;
    [[nodiscard]] virtual bool has_persistent_data(
        PersistentDataKind kind) const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> export_persistent_data(
        PersistentDataKind kind) const = 0;
    virtual void import_persistent_data(PersistentDataKind kind,
                                        const std::vector<std::uint8_t>& data) = 0;

    [[nodiscard]] virtual bool rumble_active() const noexcept { return false; }
    virtual void set_camera_frame(const std::uint8_t*, std::size_t) noexcept {}
    virtual void set_compatibility_colors(bool) noexcept {}
};

} // namespace gbb
