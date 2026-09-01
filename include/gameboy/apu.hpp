#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace gameboy {

class SaveStateCodec;
enum class HardwareModel;

class Apu {
public:
    static constexpr unsigned sample_rate = 48000;

    void initialize_post_boot(HardwareModel model,
                              bool divider_apu_signal = false) noexcept;

    [[nodiscard]] static bool handles_register(std::uint16_t address) noexcept;
    [[nodiscard]] std::uint8_t read_register(std::uint16_t address) const noexcept;
    void write_register(std::uint16_t address, std::uint8_t value,
                        bool divider_apu_signal = false) noexcept;

    void tick(unsigned cycles) noexcept;
    void clock_frame_sequencer() noexcept;
    [[nodiscard]] std::vector<std::int16_t> take_samples();
    [[nodiscard]] std::uint8_t pcm12() const noexcept;
    [[nodiscard]] std::uint8_t pcm34() const noexcept;

private:
    friend class SaveStateCodec;

    struct EnvelopeState {
        bool running{};
        bool locked{};
        std::uint8_t volume{};
        std::uint8_t timer{};
    };

    struct PulseState {
        bool enabled{};
        bool dac_enabled{};
        bool length_enabled{};
        std::uint8_t length{};
        std::uint8_t duty_step{};
        unsigned timer{};
        EnvelopeState envelope{};
    };

    struct WaveState {
        bool enabled{};
        bool dac_enabled{};
        bool length_enabled{};
        std::uint16_t length{};
        std::uint8_t position{};
        std::uint8_t sample{};
        unsigned timer{};
        bool clock_phase{};
        bool wave_ram_accessible{};
    };

    struct NoiseState {
        bool enabled{};
        bool dac_enabled{};
        bool length_enabled{};
        std::uint8_t length{};
        std::uint16_t lfsr = 0x7FFF;
        unsigned timer{};
        EnvelopeState envelope{};
    };

    void power_off() noexcept;
    void trigger_pulse1() noexcept;
    void trigger_pulse2() noexcept;
    void trigger_wave() noexcept;
    void trigger_noise() noexcept;
    static void trigger_envelope(EnvelopeState& envelope,
                                 std::uint8_t register_value) noexcept;
    static void apply_envelope_write_glitch(EnvelopeState& envelope,
                                            std::uint8_t value,
                                            std::uint8_t old_value) noexcept;
    void clock_length() noexcept;
    void clock_sweep() noexcept;
    void clock_envelopes() noexcept;
    static void clock_envelope(EnvelopeState& envelope,
                               std::uint8_t register_value) noexcept;
    void tick_pulse(PulseState& pulse, unsigned register_offset) noexcept;
    void tick_wave() noexcept;
    void tick_noise() noexcept;
    void integrate_sample(float left, float right) noexcept;
    void emit_sample(float left, float right);
    [[nodiscard]] bool next_step_skips_length() const noexcept;
    [[nodiscard]] bool any_dac_enabled() const noexcept;
    float high_pass(float input, bool dacs_enabled,
                    float& capacitor) const noexcept;
    [[nodiscard]] unsigned pulse_period(unsigned register_offset) const noexcept;
    [[nodiscard]] unsigned wave_period() const noexcept;
    [[nodiscard]] unsigned noise_period() const noexcept;
    [[nodiscard]] unsigned calculate_sweep_frequency() noexcept;
    [[nodiscard]] float pulse_output(const PulseState& pulse,
                                     unsigned register_offset) const noexcept;
    [[nodiscard]] float wave_output() const noexcept;
    [[nodiscard]] float noise_output() const noexcept;
    [[nodiscard]] unsigned pulse_digital(const PulseState& pulse,
                                         unsigned register_offset) const noexcept;
    [[nodiscard]] unsigned wave_digital() const noexcept;
    [[nodiscard]] unsigned noise_digital() const noexcept;

    std::array<std::uint8_t, 0x17> registers_{};
    std::array<std::uint8_t, 0x10> wave_ram_{};
    std::vector<std::int16_t> samples_{};
    bool cgb_hardware_{};
    bool powered_{};
    PulseState pulse1_{};
    PulseState pulse2_{};
    WaveState wave_{};
    NoiseState noise_{};
    std::uint16_t sweep_shadow_frequency_{};
    std::uint8_t sweep_timer_{};
    bool sweep_enabled_{};
    bool sweep_negated_{};
    std::uint8_t frame_sequencer_step_{};
    bool skip_frame_sequencer_event_{};
    unsigned sample_accumulator_{};
    float left_capacitor_{};
    float right_capacitor_{};
    float sample_integrator_left_{};
    float sample_integrator_right_{};
};

} // namespace gameboy
