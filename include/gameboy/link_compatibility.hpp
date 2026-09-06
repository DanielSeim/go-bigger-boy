#pragma once

#include <cstdint>

namespace gameboy {

// Software-level link families.  The serial hardware is shared by all Game
// Boy models, but Pokémon selects a different protocol after the initial
// cable handshake (Gen I, Gen II, or the Gen II Time Capsule bridge).
enum class LinkGeneration : std::uint8_t {
    unknown = 0,
    gen1 = 1,
    gen2 = 2,
};

enum class LinkRegion : std::uint8_t {
    unknown = 0,
    western = 1,
    japanese = 2,
    korean = 3,
};

enum class LinkMode : std::uint8_t {
    none = 0,
    gen1_cable_club = 1U << 0U,
    gen2_cable_club = 1U << 1U,
    time_capsule = 1U << 2U,
};

constexpr std::uint8_t operator|(const LinkMode left,
                                  const LinkMode right) noexcept {
    return static_cast<std::uint8_t>(left) |
           static_cast<std::uint8_t>(right);
}

struct LinkCompatibilityProfile {
    static constexpr std::uint8_t current_version = 1;

    std::uint8_t version{};
    LinkGeneration generation{LinkGeneration::unknown};
    LinkRegion region{LinkRegion::unknown};
    std::uint8_t modes{};

    [[nodiscard]] constexpr bool known() const noexcept {
        return version != 0 && generation != LinkGeneration::unknown &&
               region != LinkRegion::unknown && modes != 0;
    }
};

[[nodiscard]] constexpr bool has_link_mode(
    const LinkCompatibilityProfile profile, const LinkMode mode) noexcept {
    return (profile.modes & static_cast<std::uint8_t>(mode)) != 0;
}

// Returns whether the two Pokémon software families can share a cable
// session.  Gen I↔Gen I uses the Cable Club protocol, Gen II↔Gen II uses the
// Gen II protocol, and Gen I↔Gen II is valid only through Time Capsule.
// Region is checked because Japanese/Korean releases use incompatible text
// and party encodings.  Unknown profiles are intentionally handled by the
// legacy compatibility-ID handshake instead of being guessed here.
[[nodiscard]] constexpr bool link_profiles_compatible(
    const LinkCompatibilityProfile local,
    const LinkCompatibilityProfile peer) noexcept {
    if (!local.known() || !peer.known() || local.version != peer.version ||
        local.region != peer.region) {
        return false;
    }
    if (local.generation == LinkGeneration::gen1 &&
        peer.generation == LinkGeneration::gen1) {
        return has_link_mode(local, LinkMode::gen1_cable_club) &&
               has_link_mode(peer, LinkMode::gen1_cable_club);
    }
    if (local.generation == LinkGeneration::gen2 &&
        peer.generation == LinkGeneration::gen2) {
        return has_link_mode(local, LinkMode::gen2_cable_club) &&
               has_link_mode(peer, LinkMode::gen2_cable_club);
    }
    if ((local.generation == LinkGeneration::gen1 &&
         peer.generation == LinkGeneration::gen2) ||
        (local.generation == LinkGeneration::gen2 &&
         peer.generation == LinkGeneration::gen1)) {
        // Time Capsule is implemented by the Gen II side; Gen I remains on
        // its ordinary Cable Club byte format while the Gen II endpoint
        // performs the conversion and validation.
        const auto gen2 = local.generation == LinkGeneration::gen2
                              ? local
                              : peer;
        return has_link_mode(gen2, LinkMode::time_capsule);
    }
    return false;
}

} // namespace gameboy
