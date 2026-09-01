#include "gameboy/apu.hpp"

#include "gameboy/hardware_model.hpp"

#include <algorithm>

namespace gameboy {
namespace {
constexpr unsigned master_clock = 4194304;
constexpr std::size_t maximum_buffered_samples = Apu::sample_rate * 2;
// A trigger does not expose the newly started waveform until the five-clock
// startup delay has elapsed. This is observable through CGB PCM12/PCM34 reads.
constexpr unsigned channel_trigger_delay = 5;

constexpr std::array<std::array<std::uint8_t, 8>, 4> duty_patterns{{
    {{0, 0, 0, 0, 0, 0, 0, 1}},
    {{1, 0, 0, 0, 0, 0, 0, 1}},
    {{1, 0, 0, 0, 0, 1, 1, 1}},
    {{0, 1, 1, 1, 1, 1, 1, 0}},
}};
} // namespace

void Apu::initialize_post_boot(const HardwareModel model) noexcept {
    cgb_hardware_ = model == HardwareModel::cgb0 ||
                    model == HardwareModel::cgb;
    power_off();
    powered_ = true;
    registers_[0x00] = 0x80; // NR10
    registers_[0x01] = 0xBF; // NR11
    registers_[0x02] = 0xF3; // NR12
    registers_[0x04] = 0xBF; // NR14
    registers_[0x06] = 0x3F; // NR21
    registers_[0x09] = 0xBF; // NR24
    registers_[0x0A] = 0x7F; // NR30
    registers_[0x0C] = 0x9F; // NR32
    registers_[0x0E] = 0xBF; // NR34
    registers_[0x13] = 0xBF; // NR44
    registers_[0x14] = 0x77; // NR50
    registers_[0x15] = 0xF3; // NR51
    pulse1_.dac_enabled = true;
    pulse1_.enabled = model != HardwareModel::sgb &&
                      model != HardwareModel::sgb2;
}

bool Apu::handles_register(const std::uint16_t address) noexcept {
    return address >= 0xFF10 && address <= 0xFF3F;
}

std::uint8_t Apu::read_register(const std::uint16_t address) const noexcept {
    if (address >= 0xFF30 && address <= 0xFF3F) {
        if (!wave_.enabled) return wave_ram_[address - 0xFF30];
        if (!cgb_hardware_ && !wave_.wave_ram_accessible) return 0xFF;
        return wave_ram_[wave_.position / 2];
    }
    if (address < 0xFF10 || address > 0xFF26) return 0xFF;

    const auto value = registers_[address - 0xFF10];
    switch (address) {
    case 0xFF10: return static_cast<std::uint8_t>(value | 0x80);
    case 0xFF11:
    case 0xFF16: return static_cast<std::uint8_t>(value | 0x3F);
    case 0xFF12:
    case 0xFF17:
    case 0xFF21:
    case 0xFF22:
    case 0xFF24:
    case 0xFF25: return value;
    case 0xFF13:
    case 0xFF18:
    case 0xFF1B:
    case 0xFF1D:
    case 0xFF20: return 0xFF;
    case 0xFF14:
    case 0xFF19:
    case 0xFF1E:
    case 0xFF23: return static_cast<std::uint8_t>(value | 0xBF);
    case 0xFF1A: return static_cast<std::uint8_t>(value | 0x7F);
    case 0xFF1C: return static_cast<std::uint8_t>(value | 0x9F);
    case 0xFF26:
        return static_cast<std::uint8_t>(
            0x70 | (powered_ ? 0x80 : 0) | (pulse1_.enabled ? 0x01 : 0) |
            (pulse2_.enabled ? 0x02 : 0) | (wave_.enabled ? 0x04 : 0) |
            (noise_.enabled ? 0x08 : 0));
    default: return 0xFF;
    }
}

void Apu::write_register(const std::uint16_t address,
                         const std::uint8_t value) noexcept {
    if (address >= 0xFF30 && address <= 0xFF3F) {
        if (!wave_.enabled) {
            wave_ram_[address - 0xFF30] = value;
        } else if (cgb_hardware_ || wave_.wave_ram_accessible) {
            wave_ram_[wave_.position / 2] = value;
        }
        return;
    }
    if (address < 0xFF10 || address > 0xFF26) return;

    if (address == 0xFF26) {
        if ((value & 0x80) == 0) {
            power_off();
        } else if (!powered_) {
            powered_ = true;
            frame_sequencer_step_ = 0;
        }
        return;
    }
    if (!powered_) {
        if (cgb_hardware_) return;
        switch (address) {
        case 0xFF11:
            pulse1_.length = static_cast<std::uint8_t>(64 - (value & 0x3F));
            break;
        case 0xFF16:
            pulse2_.length = static_cast<std::uint8_t>(64 - (value & 0x3F));
            break;
        case 0xFF1B:
            wave_.length = static_cast<std::uint16_t>(256 - value);
            break;
        case 0xFF20:
            noise_.length = static_cast<std::uint8_t>(64 - (value & 0x3F));
            break;
        default: break;
        }
        return;
    }

    const auto index = static_cast<std::size_t>(address - 0xFF10);
    const auto old_value = registers_[index];
    registers_[index] = value;
    switch (address) {
    case 0xFF10:
        if ((old_value & 0x08) != 0 && (value & 0x08) == 0 && sweep_negated_) {
            pulse1_.enabled = false;
        }
        break;
    case 0xFF11:
        pulse1_.length = static_cast<std::uint8_t>(64 - (value & 0x3F));
        break;
    case 0xFF12:
        pulse1_.dac_enabled = (value & 0xF8) != 0;
        if (!pulse1_.dac_enabled) pulse1_.enabled = false;
        else if (cgb_hardware_ && pulse1_.enabled)
            apply_envelope_write_glitch(pulse1_.envelope, value, old_value);
        break;
    case 0xFF14:
        if (!pulse1_.length_enabled && (value & 0x40) != 0 &&
            next_step_skips_length() && pulse1_.length != 0 &&
            --pulse1_.length == 0 && (value & 0x80) == 0) {
            pulse1_.enabled = false;
        }
        pulse1_.length_enabled = (value & 0x40) != 0;
        if ((value & 0x80) != 0) trigger_pulse1();
        break;
    case 0xFF16:
        pulse2_.length = static_cast<std::uint8_t>(64 - (value & 0x3F));
        break;
    case 0xFF17:
        pulse2_.dac_enabled = (value & 0xF8) != 0;
        if (!pulse2_.dac_enabled) pulse2_.enabled = false;
        else if (cgb_hardware_ && pulse2_.enabled)
            apply_envelope_write_glitch(pulse2_.envelope, value, old_value);
        break;
    case 0xFF19:
        if (!pulse2_.length_enabled && (value & 0x40) != 0 &&
            next_step_skips_length() && pulse2_.length != 0 &&
            --pulse2_.length == 0 && (value & 0x80) == 0) {
            pulse2_.enabled = false;
        }
        pulse2_.length_enabled = (value & 0x40) != 0;
        if ((value & 0x80) != 0) trigger_pulse2();
        break;
    case 0xFF1A:
        wave_.dac_enabled = (value & 0x80) != 0;
        if (!wave_.dac_enabled) wave_.enabled = false;
        break;
    case 0xFF1B:
        wave_.length = static_cast<std::uint16_t>(256 - value);
        break;
    case 0xFF1E:
        if (!wave_.length_enabled && (value & 0x40) != 0 &&
            next_step_skips_length() && wave_.length != 0 &&
            --wave_.length == 0 && (value & 0x80) == 0) {
            wave_.enabled = false;
        }
        wave_.length_enabled = (value & 0x40) != 0;
        if ((value & 0x80) != 0) trigger_wave();
        break;
    case 0xFF20:
        noise_.length = static_cast<std::uint8_t>(64 - (value & 0x3F));
        break;
    case 0xFF21:
        noise_.dac_enabled = (value & 0xF8) != 0;
        if (!noise_.dac_enabled) noise_.enabled = false;
        else if (cgb_hardware_ && noise_.enabled)
            apply_envelope_write_glitch(noise_.envelope, value, old_value);
        break;
    case 0xFF23:
        if (!noise_.length_enabled && (value & 0x40) != 0 &&
            next_step_skips_length() && noise_.length != 0 &&
            --noise_.length == 0 && (value & 0x80) == 0) {
            noise_.enabled = false;
        }
        noise_.length_enabled = (value & 0x40) != 0;
        if ((value & 0x80) != 0) trigger_noise();
        break;
    default: break;
    }
}

void Apu::tick(const unsigned cycles) noexcept {
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        tick_pulse(pulse1_, 0x01);
        tick_pulse(pulse2_, 0x06);
        tick_wave();
        tick_noise();

        const std::array<float, 4> outputs{
            powered_ ? pulse_output(pulse1_, 0x01) : 0.0F,
            powered_ ? pulse_output(pulse2_, 0x06) : 0.0F,
            powered_ ? wave_output() : 0.0F,
            powered_ ? noise_output() : 0.0F,
        };
        const auto routing = registers_[0x15];
        auto left = 0.0F;
        auto right = 0.0F;
        for (std::size_t channel = 0; channel < outputs.size(); ++channel) {
            if ((routing & (1U << channel)) != 0) right += outputs[channel];
            if ((routing & (1U << (channel + 4))) != 0) left += outputs[channel];
        }
        const auto volumes = registers_[0x14];
        left *= static_cast<float>(((volumes >> 4) & 7) + 1) / 8.0F;
        right *= static_cast<float>((volumes & 7) + 1) / 8.0F;
        const auto dacs_enabled = any_dac_enabled();
        if (!dacs_enabled) {
            // Disconnected DACs cannot contribute a pending partial sample.
            sample_integrator_left_ = 0.0F;
            sample_integrator_right_ = 0.0F;
        }
        integrate_sample(high_pass(left, dacs_enabled, left_capacitor_),
                         high_pass(right, dacs_enabled, right_capacitor_));
    }
}

void Apu::clock_frame_sequencer() noexcept {
    if (powered_) {
        if ((frame_sequencer_step_ & 1) == 0) clock_length();
        if (frame_sequencer_step_ == 2 || frame_sequencer_step_ == 6) {
            clock_sweep();
        }
        if (frame_sequencer_step_ == 7) clock_envelopes();
    }
    frame_sequencer_step_ = static_cast<std::uint8_t>(
        (frame_sequencer_step_ + 1) & 0x07);
}

std::vector<std::int16_t> Apu::take_samples() {
    std::vector<std::int16_t> output;
    output.swap(samples_);
    samples_.reserve(4096);
    return output;
}

std::uint8_t Apu::pcm12() const noexcept {
    if (!cgb_hardware_) return 0xFF;
    return static_cast<std::uint8_t>(
        pulse_digital(pulse1_, 0x01) |
        (pulse_digital(pulse2_, 0x06) << 4));
}

std::uint8_t Apu::pcm34() const noexcept {
    if (!cgb_hardware_) return 0xFF;
    return static_cast<std::uint8_t>(wave_digital() |
                                     (noise_digital() << 4));
}

void Apu::power_off() noexcept {
    const auto pulse1_length = pulse1_.length;
    const auto pulse2_length = pulse2_.length;
    const auto wave_length = wave_.length;
    const auto noise_length = noise_.length;
    powered_ = false;
    std::fill(registers_.begin(), registers_.end(), 0);
    pulse1_ = {};
    pulse2_ = {};
    wave_ = {};
    noise_ = {};
    noise_.lfsr = 0x7FFF;
    if (!cgb_hardware_) {
        pulse1_.length = pulse1_length;
        pulse2_.length = pulse2_length;
        wave_.length = wave_length;
        noise_.length = noise_length;
    }
    sweep_shadow_frequency_ = 0;
    sweep_timer_ = 0;
    sweep_enabled_ = false;
    sweep_negated_ = false;
    frame_sequencer_step_ = 0;
    sample_integrator_left_ = 0.0F;
    sample_integrator_right_ = 0.0F;
}

void Apu::trigger_pulse1() noexcept {
    pulse1_.enabled = pulse1_.dac_enabled;
    if (pulse1_.length == 0) {
        pulse1_.length = static_cast<std::uint8_t>(
            64 - (pulse1_.length_enabled && next_step_skips_length() ? 1 : 0));
    }
    pulse1_.timer = (pulse1_.timer & 3U) | (pulse_period(0x01) & ~3U);
    pulse1_.timer += channel_trigger_delay;
    trigger_envelope(pulse1_.envelope, registers_[0x02]);
    if (frame_sequencer_step_ == 7 && pulse1_.envelope.running) {
        ++pulse1_.envelope.timer;
    }

    sweep_shadow_frequency_ = static_cast<std::uint16_t>(
        static_cast<unsigned>(registers_[0x03]) |
        (static_cast<unsigned>(registers_[0x04] & 7) << 8));
    const auto pace = static_cast<std::uint8_t>((registers_[0x00] >> 4) & 7);
    const auto shift = static_cast<std::uint8_t>(registers_[0x00] & 7);
    sweep_timer_ = pace == 0 ? 8 : pace;
    sweep_enabled_ = pace != 0 || shift != 0;
    sweep_negated_ = false;
    if (shift != 0 && calculate_sweep_frequency() > 2047) {
        pulse1_.enabled = false;
    }
}

void Apu::trigger_pulse2() noexcept {
    pulse2_.enabled = pulse2_.dac_enabled;
    if (pulse2_.length == 0) {
        pulse2_.length = static_cast<std::uint8_t>(
            64 - (pulse2_.length_enabled && next_step_skips_length() ? 1 : 0));
    }
    pulse2_.timer = (pulse2_.timer & 3U) | (pulse_period(0x06) & ~3U);
    pulse2_.timer += channel_trigger_delay;
    trigger_envelope(pulse2_.envelope, registers_[0x07]);
    if (frame_sequencer_step_ == 7 && pulse2_.envelope.running) {
        ++pulse2_.envelope.timer;
    }
}

void Apu::trigger_wave() noexcept {
    if (!cgb_hardware_ && wave_.enabled && wave_.timer == 0) {
        const auto current_byte = static_cast<std::size_t>(
            ((wave_.position + 1) & 31) / 2);
        if (current_byte < 4) {
            wave_ram_[0] = wave_ram_[current_byte];
        } else {
            const auto source = current_byte & ~std::size_t{3};
            const std::array<std::uint8_t, 4> copied{
                wave_ram_[source], wave_ram_[source + 1],
                wave_ram_[source + 2], wave_ram_[source + 3],
            };
            std::copy(copied.begin(), copied.end(), wave_ram_.begin());
        }
    }
    wave_.enabled = wave_.dac_enabled;
    if (wave_.length == 0) {
        wave_.length = static_cast<std::uint16_t>(
            256 - (wave_.length_enabled && next_step_skips_length() ? 1 : 0));
    }
    wave_.timer = wave_period() + 3;
    wave_.position = 0;
    wave_.wave_ram_accessible = cgb_hardware_;
}

void Apu::trigger_noise() noexcept {
    noise_.enabled = noise_.dac_enabled;
    if (noise_.length == 0) {
        noise_.length = static_cast<std::uint8_t>(
            64 - (noise_.length_enabled && next_step_skips_length() ? 1 : 0));
    }
    noise_.timer = noise_period();
    noise_.lfsr = 0x7FFF;
    trigger_envelope(noise_.envelope, registers_[0x11]);
    if (frame_sequencer_step_ == 7 && noise_.envelope.running) {
        ++noise_.envelope.timer;
    }
}

void Apu::trigger_envelope(EnvelopeState& envelope,
                           const std::uint8_t register_value) noexcept {
    envelope.locked = false;
    envelope.volume = static_cast<std::uint8_t>(register_value >> 4);
    const auto period = static_cast<std::uint8_t>(register_value & 7);
    envelope.timer = period == 0 ? 8 : period;
    envelope.running = period != 0;
}

void Apu::apply_envelope_write_glitch(EnvelopeState& envelope,
                                      const std::uint8_t value,
                                      const std::uint8_t old_value) noexcept {
    // CGB-D/E implement NRx2 writes as a write to the envelope counter's
    // combinational input ("zombie mode"), rather than loading the volume
    // nibble.  This is the behavior exercised by SameSuite's CGB volume
    // tests.  The pre-CGB variants have different, partly nondeterministic
    // behavior and are intentionally left on the simpler model.
    if (envelope.running) envelope.timer = static_cast<std::uint8_t>(value & 7);

    auto should_tick = (value & 7) != 0 && (old_value & 7) == 0 &&
                       !envelope.locked;
    const auto should_invert = ((value ^ old_value) & 8) != 0;
    if ((value & 0x0F) == 8 && (old_value & 0x0F) == 8 && !envelope.locked)
        should_tick = true;

    if (should_invert) {
        if ((value & 8) != 0) {
            if ((old_value & 7) == 0 && !envelope.locked) {
                envelope.volume ^= 0x0F;
            } else {
                envelope.volume = static_cast<std::uint8_t>(
                    (0x0E - envelope.volume) & 0x0F);
            }
            should_tick = false;
        } else {
            envelope.volume = static_cast<std::uint8_t>(
                (0x10 - envelope.volume) & 0x0F);
        }
    }
    if (should_tick) {
        if ((value & 8) != 0)
            envelope.volume = static_cast<std::uint8_t>(
                (envelope.volume + 1) & 0x0F);
        else
            envelope.volume = static_cast<std::uint8_t>(
                (envelope.volume - 1) & 0x0F);
    } else if ((value & 7) == 0 && envelope.running) {
        envelope.running = false;
    }
}

void Apu::clock_length() noexcept {
    const auto clock_pulse = [](PulseState& pulse) {
        if (pulse.length_enabled && pulse.length != 0 && --pulse.length == 0) {
            pulse.enabled = false;
        }
    };
    clock_pulse(pulse1_);
    clock_pulse(pulse2_);
    if (wave_.length_enabled && wave_.length != 0 && --wave_.length == 0) {
        wave_.enabled = false;
    }
    if (noise_.length_enabled && noise_.length != 0 && --noise_.length == 0) {
        noise_.enabled = false;
    }
}

void Apu::clock_sweep() noexcept {
    if (sweep_timer_ > 0) --sweep_timer_;
    if (sweep_timer_ != 0) return;

    const auto pace = static_cast<std::uint8_t>((registers_[0x00] >> 4) & 7);
    const auto shift = static_cast<std::uint8_t>(registers_[0x00] & 7);
    sweep_timer_ = pace == 0 ? 8 : pace;
    if (!sweep_enabled_ || pace == 0) return;

    const auto frequency = calculate_sweep_frequency();
    if (frequency > 2047) {
        pulse1_.enabled = false;
        return;
    }
    if (shift == 0) return;
    sweep_shadow_frequency_ = static_cast<std::uint16_t>(frequency);
    registers_[0x03] = static_cast<std::uint8_t>(frequency);
    registers_[0x04] = static_cast<std::uint8_t>(
        (registers_[0x04] & 0xF8) | ((frequency >> 8) & 7));
    if (calculate_sweep_frequency() > 2047) pulse1_.enabled = false;
}

void Apu::clock_envelopes() noexcept {
    clock_envelope(pulse1_.envelope, registers_[0x02]);
    clock_envelope(pulse2_.envelope, registers_[0x07]);
    clock_envelope(noise_.envelope, registers_[0x11]);
}

void Apu::clock_envelope(EnvelopeState& envelope,
                         const std::uint8_t register_value) noexcept {
    if (!envelope.running || envelope.locked || envelope.timer == 0) return;
    if (--envelope.timer != 0) return;

    const auto period = static_cast<std::uint8_t>(register_value & 7);
    envelope.timer = period == 0 ? 8 : period;
    const auto increase = (register_value & 0x08) != 0;
    if (increase && envelope.volume < 15) {
        ++envelope.volume;
    } else if (!increase && envelope.volume > 0) {
        --envelope.volume;
    } else {
        envelope.running = false;
        envelope.locked = true;
    }
}

void Apu::tick_pulse(PulseState& pulse,
                     const unsigned register_offset) noexcept {
    if (!pulse.enabled) return;
    if (pulse.timer > 0) --pulse.timer;
    if (pulse.timer == 0) {
        pulse.timer = pulse_period(register_offset);
        pulse.duty_step = static_cast<std::uint8_t>((pulse.duty_step + 1) & 7);
    }
}

void Apu::tick_wave() noexcept {
    if (!wave_.enabled) return;
    wave_.clock_phase = !wave_.clock_phase;
    if (wave_.clock_phase) return;

    wave_.wave_ram_accessible = false;
    if (wave_.timer == 0) {
        wave_.timer = wave_period();
        wave_.position = static_cast<std::uint8_t>((wave_.position + 1) & 31);
        const auto packed = wave_ram_[wave_.position / 2];
        wave_.sample = static_cast<std::uint8_t>(
            (wave_.position & 1) == 0 ? packed >> 4 : packed & 0x0F);
        wave_.wave_ram_accessible = wave_.enabled;
    } else {
        --wave_.timer;
    }
}

void Apu::tick_noise() noexcept {
    if (!noise_.enabled) return;
    if ((registers_[0x12] >> 4) >= 14) return;
    if (noise_.timer > 0) --noise_.timer;
    if (noise_.timer != 0) return;

    noise_.timer = noise_period();
    const auto feedback = static_cast<std::uint16_t>(
        (noise_.lfsr & 1) ^ ((noise_.lfsr >> 1) & 1));
    noise_.lfsr = static_cast<std::uint16_t>((noise_.lfsr >> 1) |
                                             (feedback << 14));
    if ((registers_[0x12] & 0x08) != 0) {
        noise_.lfsr = static_cast<std::uint16_t>(
            (noise_.lfsr & ~(1U << 6)) | (feedback << 6));
    }
}

void Apu::integrate_sample(const float left, const float right) noexcept {
    const auto next_accumulator = sample_accumulator_ + sample_rate;
    if (next_accumulator < master_clock) {
        sample_integrator_left_ += left * static_cast<float>(sample_rate);
        sample_integrator_right_ += right * static_cast<float>(sample_rate);
        sample_accumulator_ = next_accumulator;
        return;
    }

    // A 48 kHz sample boundary usually falls between two 4.19 MHz master
    // clock cycles. Split the boundary cycle so both sides are weighted by
    // their exact duration rather than selecting one instantaneous value.
    const auto before_boundary = master_clock - sample_accumulator_;
    sample_integrator_left_ += left * static_cast<float>(before_boundary);
    sample_integrator_right_ += right * static_cast<float>(before_boundary);
    emit_sample(sample_integrator_left_ / static_cast<float>(master_clock),
                sample_integrator_right_ / static_cast<float>(master_clock));

    sample_accumulator_ = next_accumulator - master_clock;
    const auto after_boundary = sample_rate - before_boundary;
    sample_integrator_left_ = left * static_cast<float>(after_boundary);
    sample_integrator_right_ = right * static_cast<float>(after_boundary);
}

void Apu::emit_sample(const float left, const float right) {
    if (samples_.size() >= maximum_buffered_samples) return;
    constexpr auto gain = 0.25F * 32767.0F;
    samples_.push_back(static_cast<std::int16_t>(
        std::clamp(left * gain, -32767.0F, 32767.0F)));
    samples_.push_back(static_cast<std::int16_t>(
        std::clamp(right * gain, -32767.0F, 32767.0F)));
}

bool Apu::next_step_skips_length() const noexcept {
    return (frame_sequencer_step_ & 1) != 0;
}

bool Apu::any_dac_enabled() const noexcept {
    return pulse1_.dac_enabled || pulse2_.dac_enabled || wave_.dac_enabled ||
           noise_.dac_enabled;
}

float Apu::high_pass(const float input, const bool dacs_enabled,
                     float& capacitor) const noexcept {
    if (!dacs_enabled) return 0.0F;
    // Capacitor charge is continuous at the master-clock rate. Applying the
    // hardware coefficient once per cycle avoids the old 48 kHz staircase.
    constexpr auto dmg_charge_factor = 0.999958F;
    constexpr auto cgb_charge_factor = 0.998943F;
    const auto charge_factor = cgb_hardware_ ? cgb_charge_factor
                                             : dmg_charge_factor;
    const auto output = input - capacitor;
    capacitor = input - output * charge_factor;
    return output;
}

unsigned Apu::pulse_period(const unsigned register_offset) const noexcept {
    const auto frequency = static_cast<unsigned>(registers_[register_offset + 2]) |
                           (static_cast<unsigned>(
                                registers_[register_offset + 3] & 7)
                            << 8);
    return (2048 - frequency) * 4;
}

unsigned Apu::wave_period() const noexcept {
    const auto frequency = static_cast<unsigned>(registers_[0x0D]) |
                           (static_cast<unsigned>(registers_[0x0E] & 7) << 8);
    return 2047 - frequency;
}

unsigned Apu::noise_period() const noexcept {
    const auto divisor_code = static_cast<unsigned>(registers_[0x12] & 7);
    const auto divisor = divisor_code == 0 ? 8U : divisor_code * 16U;
    return divisor << (registers_[0x12] >> 4);
}

unsigned Apu::calculate_sweep_frequency() noexcept {
    const auto shift = static_cast<unsigned>(registers_[0x00] & 7);
    const auto delta = static_cast<unsigned>(sweep_shadow_frequency_) >> shift;
    if ((registers_[0x00] & 0x08) != 0) {
        sweep_negated_ = true;
        return static_cast<unsigned>(sweep_shadow_frequency_) - delta;
    }
    return static_cast<unsigned>(sweep_shadow_frequency_) + delta;
}

float Apu::pulse_output(const PulseState& pulse,
                        const unsigned register_offset) const noexcept {
    if (!pulse.dac_enabled) return 0.0F;
    const auto digital = pulse_digital(pulse, register_offset);
    return 1.0F - static_cast<float>(digital) * (2.0F / 15.0F);
}

unsigned Apu::pulse_digital(const PulseState& pulse,
                            const unsigned register_offset) const noexcept {
    const auto duty = static_cast<std::size_t>(registers_[register_offset] >> 6);
    const auto high = duty_patterns[duty][pulse.duty_step] != 0;
    return pulse.enabled && pulse.dac_enabled && high ? pulse.envelope.volume : 0;
}

float Apu::wave_output() const noexcept {
    if (!wave_.dac_enabled) return 0.0F;
    const auto digital = wave_digital();
    return 1.0F - static_cast<float>(digital) * (2.0F / 15.0F);
}

unsigned Apu::wave_digital() const noexcept {
    const auto level = static_cast<unsigned>((registers_[0x0C] >> 5) & 3);
    return !wave_.enabled || !wave_.dac_enabled || level == 0
               ? 0U
               : static_cast<unsigned>(wave_.sample) >> (level - 1);
}

float Apu::noise_output() const noexcept {
    if (!noise_.dac_enabled) return 0.0F;
    const auto digital = noise_digital();
    return 1.0F - static_cast<float>(digital) * (2.0F / 15.0F);
}

unsigned Apu::noise_digital() const noexcept {
    return noise_.enabled && noise_.dac_enabled && (noise_.lfsr & 1) == 0
               ? noise_.envelope.volume
               : 0;
}

} // namespace gameboy
