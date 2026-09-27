#pragma once

#include <cstdint>

namespace gameboy {

class SnesDspEnvelope;
class SnesDspKeyOnSequence;
class SnesDspSampleRing;

// Sample-level KON/KOFF polling for eight voices. This intentionally does not
// model SPC700-cycle write windows or the staggered S3c phase of each voice.
class SnesDspKeyControl final {
public:
    struct Sample {
        std::uint8_t key_on{};
        std::uint8_t key_off{};
        bool soft_reset{};
    };

    void reset(bool poll_first_sample = true) noexcept;
    // A second KON write before the next poll replaces the first. A sampled
    // KON pulse is consumed once; KOFF and FLG persist until rewritten.
    void write_kon(std::uint8_t mask) noexcept { pending_kon_ = mask; }
    void write_koff(std::uint8_t mask) noexcept { koff_register_ = mask; }
    void write_soft_reset(bool enabled) noexcept { soft_reset_ = enabled; }
    [[nodiscard]] Sample next_sample() noexcept;

    // Apply S3c ordering: KOFF/FLG first, then an accepted KON. A soft reset
    // on the following sample still silences that voice. BRR decoding itself
    // continues through KOFF and FLG, so the sequencer is not cancelled.
    static void apply_voice(unsigned voice, const Sample& sample,
                            SnesDspSampleRing& ring,
                            SnesDspEnvelope& envelope,
                            SnesDspKeyOnSequence& sequence) noexcept;

private:
    std::uint8_t pending_kon_{};
    std::uint8_t koff_register_{};
    std::uint8_t sampled_koff_{};
    bool soft_reset_{};
    bool poll_next_{true};
};

} // namespace gameboy
