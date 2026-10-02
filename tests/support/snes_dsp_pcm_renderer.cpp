#include "snes_dsp_pcm_renderer.hpp"

#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_dsp_voice_math.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace sgb_test {
namespace {

std::uint8_t voice_register(const gameboy::SnesApuBus& bus,
                            const unsigned voice,
                            const unsigned offset) noexcept {
    return bus.dsp_register(static_cast<std::uint8_t>(voice * 0x10U + offset));
}

std::int32_t floor_div(const std::int32_t value,
                       const std::int32_t divisor) noexcept {
    return value >= 0 ? value / divisor : -(((-value) + divisor - 1) / divisor);
}

std::int16_t wrap_16(const std::int32_t value) noexcept {
    const auto bits = static_cast<std::uint32_t>(value) & 0xFFFFU;
    return static_cast<std::int16_t>(bits < 0x8000U
        ? static_cast<std::int32_t>(bits) : static_cast<std::int32_t>(bits) - 0x10000);
}

std::int16_t clamp_16(const std::int32_t value) noexcept {
    return static_cast<std::int16_t>(std::clamp(value, -32768, 32767));
}

std::int8_t signed_register(const std::uint8_t value) noexcept {
    return static_cast<std::int8_t>(static_cast<int>(value) -
                                    (value >= 0x80U ? 256 : 0));
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
    timed_endx_visible_ = 0;
    timed_pmon_ = 0;
    timed_non_ = 0;
    timed_eon_ = 0;
    timed_dir_ = 0;
    timed_feedback_ = 0;
    timed_fir_.fill(0);
    timed_echo_enabled_ = 0;
    timed_mode_ = false;
    current_keys_ = {};
    voice_output16_.fill(0);
    timed_voice_registers_.fill({});
    pending_mix_ = {};
    pending_echo_send_ = {};
    timed_echo_writeback_ = {};
    timed_echo_address_ = 0;
    timed_echo_write_pending_ = false;
    echo_history_.fill({});
    echo_offset_ = 0;
    echo_length_ = 0;
    echo_history_position_ = 0;
    echo_esa_ = 0;
    noise_ = 0x4000;
    for (auto& voice : voices_) {
        voice.stream.reset();
        voice.ring.reset();
        voice.envelope.reset();
        voice.sequence.reset();
        voice.started = false;
    }
}

SnesDspPcmRenderer::StereoSample SnesDspPcmRenderer::mix_echo(
    const StereoSample dac_mix, const StereoSample dac_send,
    const std::uint8_t master_left, const std::uint8_t master_right,
    const std::uint8_t echo_volume_left,
    const std::uint8_t echo_volume_right, const bool timed) noexcept {
    const auto address = static_cast<std::uint16_t>(
        (static_cast<unsigned>(echo_esa_) << 8U) + echo_offset_);
    const auto read_echo = [&](const unsigned channel) {
        const auto base = static_cast<std::uint16_t>(address + 2U * channel);
        const auto bits = static_cast<std::uint16_t>(
            bus_.dsp_read_ram(base) |
            (static_cast<unsigned>(bus_.dsp_read_ram(
                static_cast<std::uint16_t>(base + 1U))) << 8U));
        return static_cast<std::int16_t>(floor_div(wrap_16(bits), 2));
    };
    echo_history_[echo_history_position_] = {read_echo(0), read_echo(1)};

    const auto fir = [&](const unsigned channel) {
        std::int32_t first_seven = 0;
        for (unsigned tap = 0; tap < 7; ++tap) {
            const auto& entry = echo_history_[
                (static_cast<unsigned>(echo_history_position_) + 1U + tap) % 8U];
            const auto value = channel == 0 ? entry.left : entry.right;
            const auto coefficient = signed_register(timed ? timed_fir_[tap] :
                bus_.dsp_register(static_cast<std::uint8_t>(0x0FU + tap * 0x10U)));
            first_seven += floor_div(value * coefficient, 64);
        }
        const auto& newest = echo_history_[echo_history_position_];
        const auto value = channel == 0 ? newest.left : newest.right;
        const auto coefficient = signed_register(timed ? timed_fir_[7] :
                                                  bus_.dsp_register(0x7F));
        const auto last = wrap_16(floor_div(value * coefficient, 64));
        const auto filtered = clamp_16(wrap_16(first_seven) + last);
        return static_cast<std::int16_t>(filtered & ~1);
    };
    const StereoSample filtered{fir(0), fir(1)};
    echo_history_position_ = static_cast<std::uint8_t>(
        (static_cast<unsigned>(echo_history_position_) + 1U) % 8U);

    const auto mix_channel = [&](const std::int16_t main,
                                 const std::int16_t echo,
                                 const std::uint8_t main_volume,
                                 const std::uint8_t echo_volume) {
        const auto dry = floor_div(main * signed_register(main_volume), 128);
        const auto wet = floor_div(echo * signed_register(echo_volume), 128);
        return clamp_16(wrap_16(dry) + wrap_16(wet));
    };
    StereoSample sample{
        mix_channel(dac_mix.left, filtered.left, master_left, echo_volume_left),
        mix_channel(dac_mix.right, filtered.right, master_right, echo_volume_right),
    };

    const auto feedback = signed_register(timed ? timed_feedback_ :
                                                  bus_.dsp_register(0x0D));
    const auto feedback_channel = [&](const std::int16_t send,
                                      const std::int16_t echo) {
        const auto value = clamp_16(send + floor_div(echo * feedback, 128));
        return static_cast<std::int16_t>(value & ~1);
    };
    const StereoSample writeback{
        feedback_channel(dac_send.left, filtered.left),
        feedback_channel(dac_send.right, filtered.right),
    };
    if (timed) {
        timed_echo_writeback_ = writeback;
        timed_echo_address_ = address;
        timed_echo_write_pending_ = true;
    } else {
        if ((bus_.dsp_register(0x6C) & 0x20U) == 0) {
            write_echo_channel(address, 0, writeback.left);
            write_echo_channel(address, 1, writeback.right);
        }
        advance_echo_address();
    }
    if ((bus_.dsp_register(0x6C) & 0x40U) != 0) sample = {};
    return sample;
}

void SnesDspPcmRenderer::write_echo_channel(const std::uint16_t address,
                                             const unsigned channel,
                                             const std::int16_t value) noexcept {
    const auto base = static_cast<std::uint16_t>(address + 2U * channel);
    const auto bits = static_cast<std::uint16_t>(value);
    bus_.dsp_write_ram(base, static_cast<std::uint8_t>(bits));
    bus_.dsp_write_ram(static_cast<std::uint16_t>(base + 1U),
                       static_cast<std::uint8_t>(bits >> 8U));
}

void SnesDspPcmRenderer::advance_echo_address() noexcept {
    if (echo_offset_ == 0) {
        echo_length_ = static_cast<std::uint16_t>(
            (bus_.dsp_register(0x7D) & 0x0FU) * 0x800U);
    }
    echo_offset_ = static_cast<std::uint16_t>(echo_offset_ + 4U);
    if (echo_offset_ >= echo_length_) echo_offset_ = 0;
    echo_esa_ = bus_.dsp_register(0x6D);
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
    return next_sample_with_output_volumes(
        bus_.dsp_register(0x0C), bus_.dsp_register(0x1C),
        bus_.dsp_register(0x2C), bus_.dsp_register(0x3C));
}

std::optional<SnesDspPcmRenderer::StereoSample>
SnesDspPcmRenderer::next_sample_with_output_volumes(
    const std::uint8_t master_left, const std::uint8_t master_right,
    const std::uint8_t echo_volume_left,
    const std::uint8_t echo_volume_right) noexcept {
    const auto dac_mix = pending_mix_;
    const auto dac_send = pending_echo_send_;
    advance_sample();
    return mix_echo(dac_mix, dac_send, master_left, master_right,
                    echo_volume_left, echo_volume_right, false);
}

std::optional<SnesDspPcmRenderer::StereoSample>
SnesDspPcmRenderer::output_timed_sample(
    const std::uint8_t master_left, const std::uint8_t master_right,
    const std::uint8_t echo_volume_left,
    const std::uint8_t echo_volume_right) noexcept {
    const auto sample = mix_echo(pending_mix_, pending_echo_send_, master_left,
                                 master_right, echo_volume_left,
                                 echo_volume_right, true);
    pending_mix_ = {};
    pending_echo_send_ = {};
    return sample;
}

void SnesDspPcmRenderer::advance_timed_sample() noexcept {
    timed_mode_ = true;
    begin_sample(true);
    advance_voice(0, true);
}

void SnesDspPcmRenderer::latch_timed_fir(const unsigned phase) noexcept {
    // Echo stages read FIR0 at 22, FIR1/2 at 23, FIR3/4/5 at 24,
    // and FIR6/7 at 25. Each coefficient is held until phase 27's mix.
    constexpr std::array<unsigned, 8> read_phase{22, 23, 23, 24, 24, 24, 25, 25};
    for (unsigned tap = 0; tap < timed_fir_.size(); ++tap) {
        if (phase == read_phase[tap]) {
            timed_fir_[tap] = bus_.dsp_register(
                static_cast<std::uint8_t>(0x0FU + tap * 0x10U));
        }
    }
}

void SnesDspPcmRenderer::timed_phase26() noexcept {
    timed_feedback_ = bus_.dsp_register(0x0D);
}

void SnesDspPcmRenderer::timed_phase27() noexcept {
    // misc_27 reads PMON before the next round of staggered voice updates.
    timed_pmon_ = static_cast<std::uint8_t>(bus_.dsp_register(0x2D) & 0xFEU);
}

void SnesDspPcmRenderer::timed_phase28() noexcept {
    // misc_28 reads NON, EON, and DIR; echo_28 samples FLG for the left write.
    timed_non_ = bus_.dsp_register(0x3D);
    timed_eon_ = bus_.dsp_register(0x4D);
    timed_dir_ = bus_.dsp_register(0x5D);
    timed_echo_enabled_ = bus_.dsp_register(0x6C);
}

void SnesDspPcmRenderer::timed_echo_phase29() noexcept {
    // The two echo RAM channels have distinct FLG read windows.
    if (timed_echo_write_pending_) {
        if ((timed_echo_enabled_ & 0x20U) == 0) {
            write_echo_channel(timed_echo_address_, 0, timed_echo_writeback_.left);
        }
        advance_echo_address();
    }
    timed_echo_enabled_ = bus_.dsp_register(0x6C);
}

void SnesDspPcmRenderer::timed_echo_phase30() noexcept {
    if (timed_echo_write_pending_ && (timed_echo_enabled_ & 0x20U) == 0) {
        write_echo_channel(timed_echo_address_, 1, timed_echo_writeback_.right);
    }
    timed_echo_write_pending_ = false;
}

void SnesDspPcmRenderer::latch_timed_voice_registers(
    const unsigned phase) noexcept {
    // S-DSP V1, V2, and V3a reads are staggered; voice 0 crosses the
    // 32-clock wrap. The external PCM corpus checks every voice's boundary.
    constexpr std::array<unsigned, 8> source_phase{
        17, 20, 31, 2, 5, 8, 11, 14};
    constexpr std::array<unsigned, 8> pitch_low_phase{
        21, 0, 3, 6, 9, 12, 15, 18};
    constexpr std::array<unsigned, 8> pitch_high_phase{
        22, 1, 4, 7, 10, 13, 16, 19};
    for (unsigned index = 0; index < voices_.size(); ++index) {
        auto& registers = timed_voice_registers_[index];
        if (phase == source_phase[index]) {
            // V1 computes the sample-directory address from the global DIR
            // latch before V3c consumes the resulting pointer.
            registers.directory = timed_dir_;
            registers.source = voice_register(bus_, index, 4);
        }
        if (phase == pitch_low_phase[index]) {
            registers.pitch_low = voice_register(bus_, index, 2);
            registers.adsr0 = voice_register(bus_, index, 5);
        }
        if (phase == pitch_high_phase[index]) {
            registers.pitch_high = voice_register(bus_, index, 3);
        }
    }
}

void SnesDspPcmRenderer::advance_timed_voice(const unsigned voice) noexcept {
    if (voice == 0 || voice >= voices_.size()) return;
    advance_voice(voice, true);
}

void SnesDspPcmRenderer::mix_timed_voice_channel(
    const unsigned voice, const unsigned channel) noexcept {
    if (voice >= voices_.size() || channel >= 2) return;
    mix_voice_channel(voice, channel);
}

void SnesDspPcmRenderer::publish_timed_endx(const unsigned voice) noexcept {
    if (voice >= 8) return;
    const auto bit = static_cast<std::uint8_t>(1U << voice);
    timed_endx_visible_ = static_cast<std::uint8_t>(
        (timed_endx_visible_ & ~bit) | (ends_.endx() & bit));
}

void SnesDspPcmRenderer::advance_sample() noexcept {
    begin_sample(false);
    pending_mix_ = {};
    pending_echo_send_ = {};
    for (unsigned index = 0; index < voices_.size(); ++index) {
        advance_voice(index, false);
    }
}

void SnesDspPcmRenderer::begin_sample(const bool timed) noexcept {
    // The test renderer must fail closed for paths that could produce audio
    // different from a full S-DSP, rather than emit a plausible wrong PCM.
    rates_.advance();
    if (rates_.event(bus_.dsp_register(0x6C) & 0x1FU)) {
        const auto feedback = static_cast<std::uint16_t>(
            ((noise_ & 1U) ^ ((noise_ >> 1U) & 1U)) << 14U);
        noise_ = static_cast<std::uint16_t>(feedback ^ (noise_ >> 1U));
    }
    current_keys_ = timed ? keys_.next_timed_sample() : keys_.next_sample();
}

void SnesDspPcmRenderer::mix_voice_channel(const unsigned index,
                                           const unsigned channel) noexcept {
    const auto bit = static_cast<std::uint8_t>(1U << index);
    const auto output = gameboy::SnesDspVoiceMath::apply_channel_volume(
        voice_output16_[index], voice_register(bus_, index, channel));
    auto& main = channel == 0 ? pending_mix_.left : pending_mix_.right;
    main = gameboy::SnesDspVoiceMath::saturating_add(main, output);
    if (((timed_mode_ ? timed_eon_ : bus_.dsp_register(0x4D)) & bit) != 0) {
        auto& echo = channel == 0 ? pending_echo_send_.left
                                  : pending_echo_send_.right;
        echo = gameboy::SnesDspVoiceMath::saturating_add(echo, output);
    }
}

void SnesDspPcmRenderer::advance_voice(const unsigned index,
                                       const bool timed) noexcept {
    auto& voice = voices_[index];
    auto keys = current_keys_;
    if (timed) keys.soft_reset = (bus_.dsp_register(0x6C) & 0x80U) != 0;
    const auto bit = static_cast<std::uint8_t>(1U << index);
    const bool accepted_kon = (keys.key_on & bit) != 0;
    const auto apply_keys = [&] {
        auto controls = keys;
        if (timed && accepted_kon) controls.key_on = 0;
        gameboy::SnesDspKeyControl::apply_voice(
            index, controls, voice.ring, voice.envelope, voice.sequence);
        if (timed && accepted_kon) voice.sequence.latch_key_on();
    };
    voice_output16_[index] = 0;
    if (!voice.started) {
        if (accepted_kon) {
            voice.started = true;
            apply_keys();
            ends_.apply_sample(index, nullptr, true, voice.envelope);
        }
        return;
    }
    const auto step = voice.sequence.next(voice.ring);
    if (timed && step.read_source) {
        voice.ring.key_on();
        voice.envelope.key_on();
    }
    const auto directory = timed ? timed_voice_registers_[index].directory
                                 : bus_.dsp_register(0x5D);
    const auto source = timed ? timed_voice_registers_[index].source
                              : voice_register(bus_, index, 4);
    if (step.read_source) voice.stream.key_on(directory, source);

    const auto non = timed ? timed_non_ : bus_.dsp_register(0x3D);
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
    voice_output16_[index] = output16;
    if (!timed) {
        mix_voice_channel(index, 0);
        mix_voice_channel(index, 1);
    }

    // S3c observes a non-looping end header even without a BRR group
    // request. This must happen after this sample's output is computed.
    if (!step.read_source &&
        (bus_.dsp_read_ram(voice.stream.next_address()) & 3U) == 1U) {
        voice.envelope.reset();
    }
    if (!accepted_kon) {
        apply_keys();
    }
    if (step.clock_envelope && !accepted_kon) {
        voice.envelope.clock(rates_, timed ? timed_voice_registers_[index].adsr0
                                          : voice_register(bus_, index, 5),
                             voice_register(bus_, index, 6),
                             voice_register(bus_, index, 7));
    }
    const gameboy::SnesDspBrrGroupStream::Result* decoded = nullptr;
    gameboy::SnesDspBrrGroupStream::Result group{};
    if (step.decode_group) {
        group = voice.stream.decode_into_ring(directory, source, voice.ring);
        decoded = &group;
    }
    if (step.advance_pitch && (!accepted_kon || timed)) {
        const auto pitch_low = timed ? timed_voice_registers_[index].pitch_low
                                     : voice_register(bus_, index, 2);
        const auto pitch_high = timed ? timed_voice_registers_[index].pitch_high
                                      : voice_register(bus_, index, 3);
        const auto pitch = static_cast<std::uint16_t>(
            pitch_low | (static_cast<unsigned>(pitch_high & 0x3FU) << 8));
        const auto previous_output = index != 0 ? voice_output16_[index - 1]
                                                 : std::int16_t{0};
        voice.ring.advance_pitch(pitch, previous_output,
            index != 0 && ((timed ? timed_pmon_ : bus_.dsp_register(0x2D)) & bit) != 0);
    }
    if (accepted_kon) {
        apply_keys();
    }
    ends_.apply_sample(index, decoded, accepted_kon, voice.envelope);
}

} // namespace sgb_test
