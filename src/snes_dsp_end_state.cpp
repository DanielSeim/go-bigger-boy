#include "gameboy/snes_dsp_end_state.hpp"

#include "gameboy/snes_dsp_envelope.hpp"

namespace gameboy {

void SnesDspEndState::apply_sample(
    const unsigned voice, const SnesDspBrrGroupStream::Result* const group,
    const bool accepted_kon, SnesDspEnvelope& envelope) noexcept {
    if (voice >= 8) return;
    const auto bit = static_cast<std::uint8_t>(1U << voice);
    if (group != nullptr) {
        if (group->release_envelope && !accepted_kon) envelope.reset();
        if (group->completed_block && group->end) endx_ |= bit;
    }
    // KON wins over a BRR end reported by the same voice step.
    if (accepted_kon) endx_ &= static_cast<std::uint8_t>(~bit);
}

} // namespace gameboy
