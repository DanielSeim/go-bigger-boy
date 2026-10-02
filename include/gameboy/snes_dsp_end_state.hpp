#pragma once

#include "gameboy/snes_dsp_brr_group_stream.hpp"

#include <cstdint>

namespace gameboy {

class SnesDspEnvelope;

// Sample-level BRR end handling and ENDX latch. The caller supplies the group
// decoded this sample (if any) and whether KON was accepted for that voice.
// This does not model S3c/S4/S7 cycle offsets or header reads without a group.
class SnesDspEndState final {
public:
    void reset() noexcept { endx_ = 0; }
    void write_endx(std::uint8_t) noexcept { endx_ = 0; }
    [[nodiscard]] std::uint8_t endx() const noexcept { return endx_; }

    void apply_sample(unsigned voice,
                      const SnesDspBrrGroupStream::Result* group,
                      bool accepted_kon,
                      SnesDspEnvelope& envelope) noexcept;

private:
    friend class SnesDspStateCodec;
    std::uint8_t endx_{};
};

} // namespace gameboy
