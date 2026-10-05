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
    // Startup-compatible GB/GBC-on-GBA profiles; runtime uses the CGB-E
    // baseline, not a claim of full AGB silicon accuracy or native GBA support.
    agb0 = 10,
    agb = 11,
};

[[nodiscard]] constexpr bool is_agb_hardware(HardwareModel model) noexcept {
    return model == HardwareModel::agb0 || model == HardwareModel::agb;
}

[[nodiscard]] constexpr bool is_cgb_hardware(HardwareModel model) noexcept {
    return model == HardwareModel::cgb0 || model == HardwareModel::cgb ||
           model == HardwareModel::cgb_c || model == HardwareModel::cgb_e || is_agb_hardware(model);
}

[[nodiscard]] constexpr HardwareModel resolve_hardware_model(
    HardwareModel requested, bool supports_cgb, bool supports_sgb) noexcept {
    return requested != HardwareModel::automatic ? requested
        : supports_cgb ? HardwareModel::cgb
        : supports_sgb ? HardwareModel::sgb : HardwareModel::dmg;
}

// NTSC SGB1 derives its Game Boy clock from the SNES master clock / 5.
// The integer Hz approximation differs from 189/44 MHz by under 0.2 ppm.
// SGB2 has its own normal-speed crystal.
[[nodiscard]] constexpr unsigned hardware_clock_rate_hz(
    const HardwareModel model) noexcept {
    return model == HardwareModel::sgb ? 4'295'455U : 4'194'304U;
}

// Concrete machine profiles exposed by the frontends and the conformance
// matrix.  `automatic` is intentionally not included: it is a cartridge
// detection policy, not a hardware revision.
// AGB startup-only profiles remain outside this silicon-accuracy matrix.
inline constexpr std::array<HardwareModel, 8> concrete_hardware_models{{
    HardwareModel::dmg0, HardwareModel::dmg, HardwareModel::mgb,
    HardwareModel::sgb, HardwareModel::sgb2, HardwareModel::cgb0,
    HardwareModel::cgb_c, HardwareModel::cgb_e,
}};
inline constexpr std::array<HardwareModel, 11> selectable_hardware_models{{
    HardwareModel::automatic, HardwareModel::dmg0, HardwareModel::dmg,
    HardwareModel::mgb, HardwareModel::sgb, HardwareModel::sgb2,
    HardwareModel::cgb0, HardwareModel::cgb_c, HardwareModel::cgb_e,
    HardwareModel::agb0, HardwareModel::agb,
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
    case HardwareModel::agb0: return "agb0";
    case HardwareModel::agb: return "agb";
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
    case HardwareModel::agb0: return "AGB-0 (startup compatibility)";
    case HardwareModel::agb: return "AGB (startup compatibility)";
    case HardwareModel::cgb: return "CGB (generic)";
    }
    return "Automatic (cartridge)";
}

} // namespace gameboy
