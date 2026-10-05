#include "gameboy/sgb_icd_gb_source.hpp"

#include "gameboy/cartridge.hpp"

#include <fstream>
#include <string>
#include <stdexcept>
#include <limits>

namespace gameboy {

SgbIcdGbSource::SgbIcdGbSource(std::vector<std::uint8_t> rom,
                                 const gameboy::DiagnosticBootRom& boot_rom,
                                 const gameboy::HardwareModel model)
    : SgbIcdGbSource(gameboy::Cartridge(std::move(rom)), boot_rom, model) {}

SgbIcdGbSource::SgbIcdGbSource(gameboy::Cartridge cartridge,
                                 const gameboy::DiagnosticBootRom& boot_rom,
                                 const gameboy::HardwareModel model)
    : gb_(std::make_unique<gameboy::Emulator>(
          std::move(cartridge), model,
          gameboy::BootRomMode::diagnostic)),
      boot_image_(boot_rom), model_(model) {
    gb_->bus().install_boot_rom(boot_image_);
    gb_->bus().debug_enable_io_trace(true);
}

void SgbIcdGbSource::set_audio_sink(AudioSink sink, AudioResetSink reset, void* context) noexcept {
    audio_sink_ = sink; audio_reset_sink_ = reset; audio_context_ = context;
    gb_->bus().apu_.set_sample_sink(sink ? [](void* opaque, std::int16_t left,
                                             std::int16_t right) noexcept {
        auto& self = *static_cast<SgbIcdGbSource*>(opaque);
        ++self.audio_samples_; ++self.audio_captured_;
        // The APU area sampler emits at ceil(n * model_hz / 48000) native
        // clocks. Map that absolute boundary, not the end of a GB instruction.
        const auto hz = hardware_clock_rate_hz(self.model_);
        const auto cycles = self.audio_gap_cycles_ + self.audio_samples_/48000*hz +
            (self.audio_samples_%48000*hz+47999)/48000;
        const auto oscillator = self.model_ == HardwareModel::sgb2 ? 20971520ULL : 21477273ULL;
        const auto scale = 21477273ULL*self.divider_;
        const auto clock = self.release_clock_ + (self.model_ != HardwareModel::sgb2
            ? cycles*self.divider_
            : cycles/oscillator*scale + (cycles%oscillator*scale+oscillator-1)/oscillator);
        self.audio_sink_(self.audio_context_, clock, left, right);
    } : static_cast<Apu::SampleSink>(nullptr), this);
}

void SgbIcdGbSource::set_input_events(const std::vector<InputEvent>& events) {
    SgbFrameInput validated;
    for (const auto& event : events)
        if (!validated.add(event.frame, event.mask))
            throw std::invalid_argument("invalid native GB input event");
    input_events_ = events;
    frame_input_ = validated;
    next_input_event_ = 0;
    held_buttons_ = 0;
    if (native_gb_input_) set_input_buttons(0);
    apply_input(0);
}

void SgbIcdGbSource::set_live_button(Button button, bool pressed) noexcept {
    // Live frontend input and deterministic replay are mutually exclusive.
    if (!input_events_.empty()) return;
    const auto bit = static_cast<unsigned>(button);
    if (bit >= 8) return;
    const auto flag = static_cast<std::uint8_t>(1U << bit);
    const auto mask = static_cast<std::uint8_t>(pressed ? held_buttons_ | flag
                                                      : held_buttons_ & ~flag);
    frame_input_.hold(mask);
    set_input_buttons(mask);
    next_gb_clock_known_=false;
}

void SgbIcdGbSource::initialize_external_boot_bus() noexcept {
    // An external boot ROM executes from reset, not from a post-boot LCD/DIV
    // image. Otherwise frames can elapse before its first LCD-enable write.
    gb_->bus().write8(0xFF40, 0);
    gb_->bus().write8(0xFF04, 0);
    gb_->bus().write8(0xFF0F, 0);
    gb_->bus().write8(0xFFFF, 0);
}

void SgbIcdGbSource::apply_input(const std::uint64_t frame) noexcept {
    if (next_input_event_ >= input_events_.size() ||
        input_events_[next_input_event_].frame != frame)
        return;
    const auto next = input_events_[next_input_event_++].mask;
    if (native_gb_input_) frame_input_.advance(frame);
    set_input_buttons(next);
    ++input_events_applied_;
    if (boot_observer_) boot_observer_(boot_observer_context_, 'N', master_snapshot_, next, frame);
}

void SgbIcdGbSource::set_input_buttons(const std::uint8_t next) noexcept {
    constexpr gameboy::Button buttons[] = {
        gameboy::Button::right, gameboy::Button::left,
        gameboy::Button::up, gameboy::Button::down,
        gameboy::Button::a, gameboy::Button::b,
        gameboy::Button::select, gameboy::Button::start};
    for (unsigned bit = 0; bit < 8; ++bit) {
        const auto flag = static_cast<std::uint8_t>(1U << bit);
        if (native_gb_input_ || (held_buttons_ & flag) != (next & flag))
            gb_->set_button(buttons[bit], (next & flag) != 0);
    }
    held_buttons_ = next;
}

void SgbIcdGbSource::complete_packet() noexcept {
    if (queued_.size() >= 32) {
        missing_address_ = 0x7000;
        return;
    }
    if (continuation_packets_ == 0) {
        const auto command = static_cast<std::uint8_t>(building_[0] >> 3);
        const auto encoded = static_cast<unsigned>(building_[0] & 7U);
        const bool bootstrap = building_[0] >= 0xF1 && building_[0] <= 0xFB && (building_[0] & 1U);
        continuation_packets_ = bootstrap ? 0 : (encoded == 0 ? 1U : encoded) - 1U;
        if (command == 0x08) {
            if (sound_commands_ == 0 && audible_sound_substitution_) {
                building_[1] = 0x03;
                building_[2] = 0x80;
                building_[3] = 0x00;
            }
            if (sound_commands_ == 0) first_sound_packet_ = building_;
            ++sound_commands_;
            const auto a = building_[1];
            const auto b = building_[2];
            if ((building_[3] & 0x0CU) != 0x0CU &&
                ((a != 0 && a < 0x80) || (b != 0 && b < 0x80) ||
                 building_[4] != 0)) {
                if (audible_sound_commands_ == 0) {
                    first_audible_sound_packet_ = building_;
                    first_audible_frame_ = completed_frames_;
                }
                ++audible_sound_commands_;
            }
        }
        if (command == 0x09) ++transfer_commands_;
    } else {
        --continuation_packets_;
    }
    queued_.push_back(building_);
    ++packets_completed_;
    building_.fill(0);
    bit_count_ = 0;
    receiving_ = false;
    packet_pending_ = false;
}

void SgbIcdGbSource::joyp_write(const std::uint8_t value) noexcept {
    switch ((value >> 4) & 3U) {
    case 3: pulse_armed_ = true; break;
    case 0:
        if (pulse_armed_) {
            if (packet_pending_) complete_packet();
            receiving_ = true;
            pulse_armed_ = false;
        }
        break;
    case 1: case 2:
        if (!pulse_armed_ || !receiving_) break;
        if (packet_pending_) {
            if (((value >> 4) & 3U) == 2U) complete_packet();
            break;
        }
        if (bit_count_ < 128) {
            if (((value >> 4) & 3U) == 1U)
                building_[bit_count_ / 8] |= static_cast<std::uint8_t>(
                    1U << (bit_count_ & 7U));
            ++bit_count_;
            packet_pending_ = bit_count_ == 128;
            pulse_armed_ = false;
        }
        break;
    }
}

void SgbIcdGbSource::complete_tile_row(const unsigned tile_row) noexcept {
    if (tile_row >= 18) return;
    auto& row = rows_[tile_row & 3U];
    for (unsigned tile = 0; tile < 20; ++tile) {
        for (unsigned line = 0; line < 8; ++line) {
            std::uint8_t low{};
            std::uint8_t high{};
            for (unsigned x = 0; x < 8; ++x) {
                const auto pixel = gb_->bus().debug_sgb_source_pixel(
                    tile * 8 + x, tile_row * 8 + line);
                low |= static_cast<std::uint8_t>((pixel & 1U) << (7U - x));
                high |= static_cast<std::uint8_t>(((pixel >> 1) & 1U) << (7U - x));
            }
            row[tile * 16 + line * 2] = low;
            row[tile * 16 + line * 2 + 1] = high;
        }
    }
    row_valid_[tile_row & 3U] = true;
}

void SgbIcdGbSource::synchronize(const std::uint64_t master_clocks) noexcept {
    master_snapshot_ = master_clocks;
    if (!released_ || master_clocks < release_clock_) return;
    if (next_gb_clock_known_ && master_clocks < next_gb_clock_) return;
    // SGB1 divides the SNES CPU oscillator. SGB2 has a dedicated
    // 20,971,520 Hz oscillator; at the normal /5 setting this yields the
    // GB's 4,194,304 Hz. Use a rational conversion to avoid accumulated
    // per-step rounding drift relative to the SNES master clock.
    const auto target = sgb_icd_target_gb_cycles(
        master_clocks - release_clock_, divider_, model_);
    while (gb_cycles_ < target && missing_address_ == 0) {
        const bool stopped = audio_sink_ && gb_->cpu().stopped() &&
            (gb_->bus().read8(0xff0f) & gb_->bus().read8(0xffff) & 0x1f) == 0;
        const auto cycles = gb_->step();
        gb_cycles_ += cycles;
        // STOP consumes scheduler time but no peripheral/APU clocks. Keep
        // that gap in the absolute sample epoch, including after restoration.
        if (stopped) audio_gap_cycles_ += cycles;
        if (!boot_reported_ && !gb_->bus().boot_rom_enabled()) {
            boot_reported_ = true;
            if (boot_observer_) boot_observer_(boot_observer_context_, 'B', master_clocks, 1, gb_cycles_);
        }
        if (gb_->frame_ready()) {
            ++completed_frames_;
            gb_->consume_frame();
            apply_input(completed_frames_);
        }
        const auto ly = gb_->bus().debug_ppu_scanline();
        if (ly != last_ly_) {
            if (ly <= 144 && ly != 0 && (ly & 7U) == 0)
                complete_tile_row(ly / 8U - 1U);
            last_ly_ = ly;
        }
        // Consume at the same instruction boundary, but retain storage rather
        // than allocating a new trace vector for the next IO write.
        auto& trace = gb_->bus().debug_io_trace_;
        for (const auto& event : trace) {
            if (event.address == 0xFF00) joyp_write(event.value);
        }
        trace.clear();
    }
    const auto next = gb_cycles_ + 1;
    if (model_ != HardwareModel::sgb2) {
        next_gb_clock_known_ = next && next <=
            (std::numeric_limits<std::uint64_t>::max() - release_clock_) / divider_;
        if (next_gb_clock_known_) next_gb_clock_ = release_clock_ + next * divider_;
        return;
    }
    constexpr auto oscillator = 20971520ULL;
    const auto scale = 21477273ULL * divider_;
    const auto tail = (next % oscillator * scale + oscillator - 1) / oscillator;
    const auto quotient = next / oscillator;
    next_gb_clock_known_ = next && tail <= std::numeric_limits<std::uint64_t>::max() - release_clock_ && quotient <=
        (std::numeric_limits<std::uint64_t>::max() - release_clock_ - tail) / scale;
    if (next_gb_clock_known_) next_gb_clock_ = release_clock_ + quotient * scale + tail;
}

bool SgbIcdGbSource::read(const std::uint16_t address,
                           const std::uint64_t master_clocks,
                           std::uint8_t& value) noexcept {
    synchronize(master_clocks);
    if (missing_address_ != 0) return false;
    switch (address) {
    case 0x6000: {
        // ICD status follows pixel scanlines, not the CPU's early LY=0 alias.
        const auto ly = gb_->bus().debug_ppu_scanline();
        value = static_cast<std::uint8_t>(
            ((ly >= 144 ? 0x11U : ly / 8U) << 3) | ((ly / 8U) & 3U));
        return true;
    }
    case 0x6002:
        value = queued_.empty() ? 0 : 1;
        return true;
    default:
        if (address >= 0x7800 && address <= 0x780F) {
            if (!row_valid_[selected_row_]) break;
            value = row_stream_offset_ < 320
                ? rows_[selected_row_][row_stream_offset_] : 0xFF;
            row_stream_offset_ = (row_stream_offset_ + 1U) & 511U;
            return true;
        }
        if (address >= 0x7000 && address <= 0x700F) {
            if (address == 0x7000) {
                if (queued_.empty()) break;
                latched_ = queued_.front();
                queued_.pop_front();
                ++packets_delivered_;
                if ((latched_[0] >> 3) == 0x08) {
                    ++sound_packets_delivered_;
                    last_delivered_sound_packet_ = latched_;
                    if (boot_observer_) {
                        const auto parameters = std::uint32_t(latched_[1]) | (std::uint32_t(latched_[2]) << 8) |
                            (std::uint32_t(latched_[3]) << 16) | (std::uint32_t(latched_[4]) << 24);
                        boot_observer_(boot_observer_context_, 'S', master_clocks, parameters, sound_packets_delivered_ - 1);
                    }
                    const auto a = latched_[1];
                    const auto b = latched_[2];
                    if ((latched_[3] & 0x0CU) != 0x0CU &&
                        ((a != 0 && a < 0x80) || (b != 0 && b < 0x80) ||
                         latched_[4] != 0))
                        ++audible_sound_packets_delivered_;
                }
            }
            value = latched_[address & 0xF];
            return true;
        }
        break;
    }
    missing_address_ = address;
    return false;
}

bool SgbIcdGbSource::write(const std::uint16_t address,
                            const std::uint64_t master_clocks,
                            const std::uint8_t value) noexcept {
    if (address == 0x6003) {
        constexpr unsigned dividers[] = {4, 5, 7, 9};
        const auto next_divider = dividers[value & 3U];
        const bool run = (value & 0x80U) != 0;
        // The bounded ICD has no piecewise oscillator phase model. Do not
        // silently retime captured audio on a live divider change.
        if (audio_sink_ && released_ && run && divider_ != next_divider) {
            missing_address_ = address; return false;
        }
        if (boot_observer_) boot_observer_(boot_observer_context_, 'C', master_clocks, value, gb_cycles_);
        ++control_writes_;
        last_control_ = value;
        if (!run) {
            const auto live_held=held_buttons_;
            released_ = false;
            gb_->reset();
            if (audio_sink_) {
                // Reset the presentation accumulator too, without changing
                // channel/register-visible behavior in the legacy path.
                gb_->set_audio_enabled(false); gb_->set_audio_enabled(true);
                audio_samples_ = 0;
                audio_gap_cycles_ = 0;
                if (audio_reset_sink_) audio_reset_sink_(audio_context_, master_clocks);
            }
            gb_->bus().install_boot_rom(boot_image_);
            if (native_gb_input_) initialize_external_boot_bus();
            gb_->bus().debug_enable_io_trace(true);
            gb_cycles_ = 0;
            boot_reported_ = false;
            completed_frames_ = 0;
            next_input_event_ = 0;
            held_buttons_ = 0;
            frame_input_.reset();
            if (native_gb_input_) set_input_buttons(0);
            apply_input(0);
            if (input_events_.empty()) {
                frame_input_.hold(live_held);
                set_input_buttons(live_held);
            }
            last_ly_ = gb_->bus().debug_ppu_scanline();
            row_valid_.fill(false);
            row_stream_offset_ = 0;
            queued_.clear();
            bit_count_ = 0;
            building_.fill(0);
            packet_pending_ = false;
            continuation_packets_ = 0;
            receiving_ = false;
            pulse_armed_ = true;
        } else if (!released_) {
            released_ = true;
            release_clock_ = master_clocks;
        }
        divider_ = next_divider;
        next_gb_clock_known_ = false;
        return true;
    }
    synchronize(master_clocks);
    if (address == 0x6001) {
        selected_row_ = value & 3U;
        row_stream_offset_ = 0;
        return true;
    }
    if (address >= 0x6004 && address <= 0x6007) {
        const auto controller = frame_input_.controller(address - 0x6004, value, native_gb_input_);
        constexpr gameboy::Button buttons[] = {
            gameboy::Button::right, gameboy::Button::left,
            gameboy::Button::up, gameboy::Button::down,
            gameboy::Button::a, gameboy::Button::b,
            gameboy::Button::select, gameboy::Button::start};
        for (unsigned bit = 0; bit < 8; ++bit)
            gb_->set_player_button(static_cast<std::uint8_t>(address - 0x6004),
                                   buttons[bit], (controller & (1U << bit)) == 0);
        return true;
    }
    missing_address_ = address;
    return false;
}

} // namespace gameboy
