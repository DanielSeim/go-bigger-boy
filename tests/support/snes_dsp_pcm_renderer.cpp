#include "snes_dsp_pcm_renderer.hpp"

#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_voice_math.hpp"

#include <cstdint>
#include <optional>

namespace sgb_test {
namespace {

std::uint8_t voice_register(const gameboy::SnesApuBus& bus,
                            const unsigned voice,
                            const unsigned offset) noexcept {
    return bus.dsp_register(static_cast<std::uint8_t>(voice * 0x10U + offset));
}

} // namespace

SnesDspPcmRenderer::SnesDspPcmRenderer(gameboy::SnesApuBus& bus) noexcept
    : bus_(bus), voices_{Voice{bus}, Voice{bus}, Voice{bus}, Voice{bus},
                         Voice{bus}, Voice{bus}, Voice{bus}, Voice{bus}} {
    reset();
}

void SnesDspPcmRenderer::reset() noexcept {
    rates_.reset();
    // The reference starts at DSP phase 0. Its first DAC output (phase 27)
    // precedes the first KON/KOFF poll (phase 30), which occurs on the
    // following sample after the every-other-sample phase toggles.
    keys_.reset(false);
    ends_.reset();
    pending_mix_ = {};
    noise_ = 0x4000;
    for (auto& voice : voices_) {
        voice.stream.reset();
        voice.ring.reset();
        voice.envelope.reset();
        voice.sequence.reset();
        voice.started = false;
    }
}

void SnesDspPcmRenderer::write_dsp(const std::uint8_t address,
                                    const std::uint8_t value) noexcept {
    if (address >= 0x80U) return;
    bus_.spc_write(0xF2, address);
    bus_.spc_write(0xF3, value);
    switch (address) {
    case 0x4C: keys_.write_kon(value); break;
    case 0x5C: keys_.write_koff(value); break;
    case 0x6C: keys_.write_soft_reset((value & 0x80U) != 0); break;
    case 0x7C: ends_.write_endx(value); break;
    default: break;
    }
}

std::optional<SnesDspPcmRenderer::StereoSample>
SnesDspPcmRenderer::next_sample() noexcept {
    // The test renderer must fail closed for paths that could produce audio
    // different from a full S-DSP, rather than emit a plausible wrong PCM.
    if (bus_.dsp_register(0x4D) != 0 || bus_.dsp_register(0x2C) != 0 ||
        bus_.dsp_register(0x3C) != 0 || bus_.dsp_register(0x0D) != 0) {
        return std::nullopt;
    }

    rates_.advance();
    if (rates_.event(bus_.dsp_register(0x6C) & 0x1FU)) {
        const auto feedback = static_cast<std::uint16_t>(
            ((noise_ & 1U) ^ ((noise_ >> 1U) & 1U)) << 14U);
        noise_ = static_cast<std::uint16_t>(feedback ^ (noise_ >> 1U));
    }
    const auto key_sample = keys_.next_sample();
    std::array<std::int16_t, 8> voice_output16{};
    std::int16_t main_left = 0;
    std::int16_t main_right = 0;
    for (unsigned index = 0; index < voices_.size(); ++index) {
        auto& voice = voices_[index];
        const auto bit = static_cast<std::uint8_t>(1U << index);
        const bool accepted_kon = (key_sample.key_on & bit) != 0;
        if (!voice.started) {
            if (accepted_kon) {
                voice.started = true;
                gameboy::SnesDspKeyControl::apply_voice(
                    index, key_sample, voice.ring, voice.envelope, voice.sequence);
                ends_.apply_sample(index, nullptr, true, voice.envelope);
            }
            continue;
        }
        const auto step = voice.sequence.next(voice.ring);
        const auto directory = bus_.dsp_register(0x5D);
        const auto source = voice_register(bus_, index, 4);
        if (step.read_source) voice.stream.key_on(directory, source);

        const auto non = bus_.dsp_register(0x3D);
        const auto noise_value = noise_;
        const auto source15 = (non & bit) != 0
            ? static_cast<std::int16_t>(
                noise_value >= 0x4000U ? static_cast<int>(noise_value) - 0x8000
                                        : static_cast<int>(noise_value))
            : voice.ring.interpolated();
        const auto output15 = step.force_silence ? std::int16_t{0} :
            gameboy::SnesDspVoiceMath::apply_envelope(
                source15, voice.envelope.value());
        const auto output16 = gameboy::SnesDspVoiceMath::expand_to_16bit(output15);
        voice_output16[index] = output16;
        const StereoSample voice_output{
            gameboy::SnesDspVoiceMath::apply_channel_volume(
                output16, voice_register(bus_, index, 0)),
            gameboy::SnesDspVoiceMath::apply_channel_volume(
                output16, voice_register(bus_, index, 1)),
        };
        main_left = gameboy::SnesDspVoiceMath::saturating_add(
            main_left, voice_output.left);
        main_right = gameboy::SnesDspVoiceMath::saturating_add(
            main_right, voice_output.right);

        // S3c observes a non-looping end header even without a BRR group
        // request. This must happen after this sample's output is computed.
        if (!step.read_source &&
            (bus_.dsp_read_ram(voice.stream.next_address()) & 3U) == 1U) {
            voice.envelope.reset();
        }
        if (!accepted_kon) {
            gameboy::SnesDspKeyControl::apply_voice(
                index, key_sample, voice.ring, voice.envelope, voice.sequence);
        }
        if (step.clock_envelope && !accepted_kon) {
            voice.envelope.clock(rates_, voice_register(bus_, index, 5),
                                 voice_register(bus_, index, 6),
                                 voice_register(bus_, index, 7));
        }
        const gameboy::SnesDspBrrGroupStream::Result* decoded = nullptr;
        gameboy::SnesDspBrrGroupStream::Result group{};
        if (step.decode_group) {
            group = voice.stream.decode_into_ring(directory, source, voice.ring);
            decoded = &group;
        }
        if (step.advance_pitch && !accepted_kon) {
            const auto pitch = static_cast<std::uint16_t>(
                voice_register(bus_, index, 2) |
                (static_cast<unsigned>(voice_register(bus_, index, 3) & 0x3FU) << 8));
            const auto previous_output = index != 0 ? voice_output16[index - 1]
                                                     : std::int16_t{0};
            voice.ring.advance_pitch(pitch, previous_output,
                index != 0 && (bus_.dsp_register(0x2D) & bit) != 0);
        }
        if (accepted_kon) {
            gameboy::SnesDspKeyControl::apply_voice(
                index, key_sample, voice.ring, voice.envelope, voice.sequence);
        }
        ends_.apply_sample(index, decoded, accepted_kon, voice.envelope);
    }
    const auto dac_mix = pending_mix_;
    pending_mix_ = StereoSample{main_left, main_right};
    StereoSample sample{
        gameboy::SnesDspVoiceMath::apply_channel_volume(
            dac_mix.left, bus_.dsp_register(0x0C)),
        gameboy::SnesDspVoiceMath::apply_channel_volume(
            dac_mix.right, bus_.dsp_register(0x1C)),
    };
    if ((bus_.dsp_register(0x6C) & 0x40U) != 0) sample = {};
    return sample;
}

} // namespace sgb_test
