#include "save_state_apu.hpp"

#include "gameboy/apu.hpp"

namespace gameboy {
namespace {

template <std::size_t Size>
void write_bytes(save_state_format::Writer& writer,
                 const std::array<std::uint8_t, Size>& values) {
    writer.bytes(values.data(), values.size());
}

template <std::size_t Size>
void read_bytes(save_state_format::Reader& reader,
                std::array<std::uint8_t, Size>& values) {
    reader.bytes(values.data(), values.size());
}

} // namespace

void SaveStateApuCodec::write(save_state_format::Writer& writer,
                              const Apu& apu) {
    const auto write_envelope = [&writer](const auto& envelope) {
        writer.boolean(envelope.running);
        writer.u8(envelope.volume);
        writer.u8(envelope.timer);
    };
    const auto write_pulse = [&writer, &write_envelope](const auto& pulse) {
        writer.boolean(pulse.enabled);
        writer.boolean(pulse.dac_enabled);
        writer.boolean(pulse.length_enabled);
        writer.u8(pulse.length);
        writer.u8(pulse.duty_step);
        writer.u32(pulse.timer);
        write_envelope(pulse.envelope);
    };

    write_bytes(writer, apu.registers_);
    write_bytes(writer, apu.wave_ram_);
    writer.boolean(apu.powered_);
    write_pulse(apu.pulse1_);
    write_pulse(apu.pulse2_);
    writer.boolean(apu.wave_.enabled);
    writer.boolean(apu.wave_.dac_enabled);
    writer.boolean(apu.wave_.length_enabled);
    writer.u16(apu.wave_.length);
    writer.u8(apu.wave_.position);
    writer.u8(apu.wave_.sample);
    writer.u32(apu.wave_.timer);
    writer.boolean(apu.wave_.clock_phase);
    writer.boolean(apu.wave_.wave_ram_accessible);
    writer.boolean(apu.noise_.enabled);
    writer.boolean(apu.noise_.dac_enabled);
    writer.boolean(apu.noise_.length_enabled);
    writer.u8(apu.noise_.length);
    writer.u16(apu.noise_.lfsr);
    writer.u32(apu.noise_.timer);
    write_envelope(apu.noise_.envelope);
    writer.u16(apu.sweep_shadow_frequency_);
    writer.u8(apu.sweep_timer_);
    writer.boolean(apu.sweep_enabled_);
    writer.boolean(apu.sweep_negated_);
    writer.u8(static_cast<std::uint8_t>(
        apu.frame_sequencer_step_ |
        (apu.skip_frame_sequencer_event_ ? 0x80 : 0)));
    writer.u32(apu.sample_accumulator_);
    writer.f32(apu.left_capacitor_);
    writer.f32(apu.right_capacitor_);
}

void SaveStateApuCodec::read(save_state_format::Reader& reader, Apu& apu,
                             const std::uint32_t /*version*/) {
    const auto read_envelope = [&reader](auto& envelope) {
        envelope.running = reader.boolean();
        // The zombie-mode lock is a short-lived combinational state and is
        // deliberately not part of the save-state wire format.
        envelope.locked = false;
        envelope.volume = reader.u8();
        envelope.timer = reader.u8();
    };
    const auto read_pulse = [&reader, &read_envelope](auto& pulse) {
        pulse.enabled = reader.boolean();
        pulse.dac_enabled = reader.boolean();
        pulse.length_enabled = reader.boolean();
        pulse.length = reader.u8();
        pulse.duty_step = reader.u8();
        pulse.timer = reader.u32();
        read_envelope(pulse.envelope);
    };

    read_bytes(reader, apu.registers_);
    read_bytes(reader, apu.wave_ram_);
    apu.samples_.clear();
    apu.powered_ = reader.boolean();
    read_pulse(apu.pulse1_);
    read_pulse(apu.pulse2_);
    apu.wave_.enabled = reader.boolean();
    apu.wave_.dac_enabled = reader.boolean();
    apu.wave_.length_enabled = reader.boolean();
    apu.wave_.length = reader.u16();
    apu.wave_.position = reader.u8();
    apu.wave_.sample = reader.u8();
    apu.wave_.timer = reader.u32();
    apu.wave_.clock_phase = reader.boolean();
    apu.wave_.wave_ram_accessible = reader.boolean();
    apu.noise_.enabled = reader.boolean();
    apu.noise_.dac_enabled = reader.boolean();
    apu.noise_.length_enabled = reader.boolean();
    apu.noise_.length = reader.u8();
    apu.noise_.lfsr = reader.u16();
    apu.noise_.timer = reader.u32();
    read_envelope(apu.noise_.envelope);
    apu.sweep_shadow_frequency_ = reader.u16();
    apu.sweep_timer_ = reader.u8();
    apu.sweep_enabled_ = reader.boolean();
    apu.sweep_negated_ = reader.boolean();
    const auto frame_sequencer_state = reader.u8();
    apu.frame_sequencer_step_ =
        static_cast<std::uint8_t>(frame_sequencer_state & 0x07);
    apu.skip_frame_sequencer_event_ = (frame_sequencer_state & 0x80) != 0;
    apu.sample_accumulator_ = reader.u32();
    apu.left_capacitor_ = reader.f32();
    apu.right_capacitor_ = reader.f32();
    apu.pulse1_.duty = static_cast<std::uint8_t>(apu.registers_[0x01] >> 6);
    apu.pulse1_.pending_duty = apu.pulse1_.duty;
    apu.pulse1_.duty_update_pending = false;
    apu.pulse2_.duty = static_cast<std::uint8_t>(apu.registers_[0x06] >> 6);
    apu.pulse2_.pending_duty = apu.pulse2_.duty;
    apu.pulse2_.duty_update_pending = false;
    apu.pulse1_.period = apu.pulse_period(0x01);
    apu.pulse1_.just_reloaded = false;
    apu.pulse2_.period = apu.pulse_period(0x06);
    apu.pulse2_.just_reloaded = false;
    apu.sample_integrator_left_ = 0.0F;
    apu.sample_integrator_right_ = 0.0F;
}

} // namespace gameboy
