#pragma once

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

} // namespace gameboy
