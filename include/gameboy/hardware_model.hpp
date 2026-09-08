#pragma once

#include <array>
#include <string_view>

namespace gameboy {

enum class HardwareModel {
    automatic,
    dmg0,
    dmg,
    mgb,
    sgb,
    sgb2,
    cgb0 = 6,
    // Keep the historical generic CGB value stable for clients that persist
    // or exchange the enum numerically.
    cgb = 7,
    // CGB-C is the last early CGB revision. It retains the pre-D envelope,
    // noise, and channel-alignment behavior documented by SameBoy.
    cgb_c = 8,
    // CGB-E represents the late CGB hardware used by the generic CGB profile.
    cgb_e = 9,
};

// Concrete machine profiles exposed by the frontends and the conformance
// matrix.  `automatic` is intentionally not included: it is a cartridge
// detection policy, not a hardware revision.
inline constexpr std::array<HardwareModel, 8> concrete_hardware_models{{
    HardwareModel::dmg0, HardwareModel::dmg, HardwareModel::mgb,
    HardwareModel::sgb, HardwareModel::sgb2, HardwareModel::cgb0,
    HardwareModel::cgb_c, HardwareModel::cgb_e,
}};
inline constexpr std::array<HardwareModel, 9> selectable_hardware_models{{
    HardwareModel::automatic, HardwareModel::dmg0, HardwareModel::dmg,
    HardwareModel::mgb, HardwareModel::sgb, HardwareModel::sgb2,
    HardwareModel::cgb0, HardwareModel::cgb_c, HardwareModel::cgb_e,
}};

[[nodiscard]] constexpr std::string_view hardware_model_id(
    const HardwareModel model) noexcept {
    switch (model) {
    case HardwareModel::automatic: return "auto";
    case HardwareModel::dmg0: return "dmg0";
    case HardwareModel::dmg: return "dmg";
    case HardwareModel::mgb: return "mgb";
    case HardwareModel::sgb: return "sgb";
    case HardwareModel::sgb2: return "sgb2";
    case HardwareModel::cgb0: return "cgb0";
    case HardwareModel::cgb_c: return "cgb-c";
    case HardwareModel::cgb_e: return "cgb-e";
    case HardwareModel::cgb: return "cgb";
    }
    return "auto";
}

[[nodiscard]] constexpr std::string_view hardware_model_name(
    const HardwareModel model) noexcept {
    switch (model) {
    case HardwareModel::automatic: return "Automatic (cartridge)";
    case HardwareModel::dmg0: return "DMG-0";
    case HardwareModel::dmg: return "DMG-B / DMG";
    case HardwareModel::mgb: return "MGB";
    case HardwareModel::sgb: return "SGB";
    case HardwareModel::sgb2: return "SGB2";
    case HardwareModel::cgb0: return "CGB-0";
    case HardwareModel::cgb_c: return "CGB-C";
    case HardwareModel::cgb_e: return "CGB-E";
    case HardwareModel::cgb: return "CGB (generic)";
    }
    return "Automatic (cartridge)";
}

} // namespace gameboy
