#include "gameboy/snes_dsp_key_on_sequence.hpp"

#include "gameboy/snes_dsp_envelope.hpp"
#include "gameboy/snes_dsp_sample_ring.hpp"

namespace gameboy {

void SnesDspKeyOnSequence::key_on(SnesDspSampleRing& ring,
                                  SnesDspEnvelope& envelope) noexcept {
    ring.key_on();
    envelope.key_on();
    stage_ = Stage::source;
}

SnesDspKeyOnSequence::Step SnesDspKeyOnSequence::next(
    const SnesDspSampleRing& ring) noexcept {
    switch (stage_) {
    case Stage::source:
        stage_ = Stage::group0;
        return Step{true, true, false, false, false};
    case Stage::group0:
        stage_ = Stage::group1;
        return Step{true, false, true, false, false};
    case Stage::group1:
        stage_ = Stage::group2;
        return Step{true, false, true, false, false};
    case Stage::group2:
        stage_ = Stage::envelope;
        return Step{true, false, true, false, false};
    case Stage::envelope:
        stage_ = Stage::steady;
        return Step{true, false, false, true, false};
    case Stage::steady:
        return Step{false, false, ring.group_due(), true, true};
    }
    return {};
}

} // namespace gameboy
