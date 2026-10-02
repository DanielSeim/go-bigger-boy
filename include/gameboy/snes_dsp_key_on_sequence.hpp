#pragma once

namespace gameboy {

class SnesDspEnvelope;
class SnesDspSampleRing;

// Sample-level actions after an accepted KON. This is not a DSP cycle
// scheduler: KON polling, the final pre-KON decode, per-voice register reads,
// and the other seven voices still require a separate host.
class SnesDspKeyOnSequence final {
public:
    struct Step {
        bool force_silence{};
        bool read_source{};
        bool decode_group{};
        bool clock_envelope{};
        bool advance_pitch{};
    };

    void reset() noexcept { stage_ = Stage::steady; }
    // Accepted KON resets ring position and envelope, preserving ring data.
    void key_on(SnesDspSampleRing& ring, SnesDspEnvelope& envelope) noexcept;
    // Cycle-driven callers latch KON at V3c without clearing the old voice
    // state yet. Reset ring/envelope at the following read_source step.
    void latch_key_on() noexcept { stage_ = Stage::source; }
    // Advance one 32 kHz output sample. The caller performs output before
    // clock_envelope/decode_group/advance_pitch; read_source is needed before
    // any preload group. During playback, decode_group reflects group_due().
    [[nodiscard]] Step next(const SnesDspSampleRing& ring) noexcept;

private:
    enum class Stage { steady, source, group0, group1, group2, envelope };
    Stage stage_{Stage::steady};
};

} // namespace gameboy
