#include "gameboy/snes_dsp_key_control.hpp"

#include "gameboy/snes_dsp_envelope.hpp"
#include "gameboy/snes_dsp_key_on_sequence.hpp"
#include "gameboy/snes_dsp_sample_ring.hpp"

namespace gameboy {

void SnesDspKeyControl::reset(const bool poll_first_sample) noexcept {
    pending_kon_ = 0;
    koff_register_ = 0;
    sampled_koff_ = 0;
    soft_reset_ = false;
    poll_next_ = poll_first_sample;
}

SnesDspKeyControl::Sample SnesDspKeyControl::next_sample() noexcept {
    std::uint8_t key_on = 0;
    if (poll_next_) {
        key_on = pending_kon_;
        pending_kon_ = 0;
        sampled_koff_ = koff_register_;
    }
    poll_next_ = !poll_next_;
    return Sample{key_on, sampled_koff_, soft_reset_};
}

void SnesDspKeyControl::apply_voice(const unsigned voice, const Sample& sample,
                                     SnesDspSampleRing& ring,
                                     SnesDspEnvelope& envelope,
                                     SnesDspKeyOnSequence& sequence) noexcept {
    if (voice >= 8) return;
    const auto bit = static_cast<std::uint8_t>(1U << voice);
    if (sample.soft_reset) {
        envelope.reset();
    } else if ((sample.key_off & bit) != 0) {
        envelope.key_off();
    }
    if ((sample.key_on & bit) != 0) sequence.key_on(ring, envelope);
}

} // namespace gameboy
