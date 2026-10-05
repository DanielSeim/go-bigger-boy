#include "gameboy/sgb_icd_gb_source.hpp"

#include "gameboy/cartridge.hpp"

#include <algorithm>
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
          gameboy::BootRomMode::replacement)),
      boot_image_(boot_rom), model_(model) {
    gb_->bus().install_boot_rom(boot_image_);
    gb_->bus().debug_enable_io_trace(true);
    bind_lcd_sink();
}

void SgbIcdGbSource::bind_lcd_sink() noexcept {
    gb_->bus().ppu_.set_sgb_lcd_sink([](void* context, std::uint64_t clock,
                                       unsigned x, unsigned y, std::uint8_t pixel) noexcept {
        auto& self = *static_cast<SgbIcdGbSource*>(context);
        self.receive_lcd_event({clock + self.lcd_clock_bias_,
            static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), pixel});
    }, this);
}

void SgbIcdGbSource::receive_lcd_event(LcdEvent event) noexcept {
    if (event.clock <= lcd_target_) { apply_lcd_event(event); return; }
    // Only the tail of one GB instruction can lie beyond a rendezvous. The
    // longest instruction is 24 clocks: fixed storage, no per-pixel allocation.
    if (pending_lcd_count_ == pending_lcd_.size()) { missing_address_ = 0x7800; return; }
    pending_lcd_[(pending_lcd_head_ + pending_lcd_count_++) % pending_lcd_.size()] = event;
}

void SgbIcdGbSource::drain_lcd_events(const std::uint64_t target) noexcept {
    while (pending_lcd_count_ && pending_lcd_[pending_lcd_head_].clock <= target) {
        apply_lcd_event(pending_lcd_[pending_lcd_head_]);
        pending_lcd_[pending_lcd_head_] = {};
        pending_lcd_head_ = (pending_lcd_head_ + 1) % pending_lcd_.size();
        --pending_lcd_count_;
    }
    if (!pending_lcd_count_) pending_lcd_head_ = 0;
}

void SgbIcdGbSource::apply_lcd_event(const LcdEvent& e) noexcept {
    if (e.x >= 160) {
        last_ly_ = e.y;
        // LCD off stops the producer, not the SNES-side ring RAM. Retain
        // complete banks while off; an interrupted active bank stays incomplete.
        if (e.x == 160 && e.y < 144 && (e.y & 7U) == 0)
            row_complete_[(e.y / 8U) & 3U] = false;
        if (e.x == 161 && e.pixel) row_complete_[0] = false;
    } else {
        const auto bank = (e.y / 8U) & 3U;
        if (e.x == 0 && (e.y & 7U) == 0) row_complete_[bank] = false;
        auto& row = rows_[bank];
        const auto offset = (e.x / 8U) * 16U + (e.y & 7U) * 2U;
        const auto bit = static_cast<std::uint8_t>(1U << (7U - (e.x & 7U)));
        row[offset] = static_cast<std::uint8_t>((row[offset] & ~bit) | ((e.pixel & 1U) ? bit : 0));
        row[offset + 1] = static_cast<std::uint8_t>((row[offset + 1] & ~bit) | ((e.pixel & 2U) ? bit : 0));
        if (e.x == 159 && (e.y & 7U) == 7) row_valid_[bank] = row_complete_[bank] = true;
    }
    if (lcd_observer_) lcd_observer_(lcd_observer_context_, e.clock, e.x, e.y, e.pixel);
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
    next_sync_clock_known_=false;
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

void SgbIcdGbSource::synchronize(const std::uint64_t master_clocks) noexcept {
    master_snapshot_ = master_clocks;
    if (!released_ || master_clocks < release_clock_) return;
    // The cached deadline covers both the next CPU instruction and the first
    // deferred physical LCD edge. Between these edges no observable work is
    // due, even when an instruction has left future pixels in the queue.
    if (next_sync_clock_known_ && master_clocks < next_sync_clock_) return;
    // SGB1 divides the SNES CPU oscillator. SGB2 has a dedicated
    // 20,971,520 Hz oscillator; at the normal /5 setting this yields the
    // GB's 4,194,304 Hz. Use a rational conversion to avoid accumulated
    // per-step rounding drift relative to the SNES master clock.
    const auto target = sgb_icd_target_gb_cycles(
        master_clocks - release_clock_, divider_, model_);
    lcd_target_ = target;
    drain_lcd_events(target);
    while (gb_cycles_ < target && missing_address_ == 0) {
        const bool stopped = audio_sink_ && gb_->cpu().stopped() &&
            (gb_->bus().read8(0xff0f) & gb_->bus().read8(0xffff) & 0x1f) == 0;
        lcd_clock_bias_ = gb_cycles_ - gb_->bus().debug_bus_cycles_;
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
        // Consume at the same instruction boundary, but retain storage rather
        // than allocating a new trace vector for the next IO write.
        auto& trace = gb_->bus().debug_io_trace_;
        for (const auto& event : trace) {
            if (event.address == 0xFF00) joyp_write(event.value);
        }
        trace.clear();
    }
    auto next = gb_cycles_ + 1;
    if (pending_lcd_count_)
        next = std::min(next, pending_lcd_[pending_lcd_head_].clock);
    if (model_ != HardwareModel::sgb2) {
        next_sync_clock_known_ = next && next <=
            (std::numeric_limits<std::uint64_t>::max() - release_clock_) / divider_;
        if (next_sync_clock_known_) next_sync_clock_ = release_clock_ + next * divider_;
        return;
    }
    constexpr auto oscillator = 20971520ULL;
    const auto scale = 21477273ULL * divider_;
    const auto tail = (next % oscillator * scale + oscillator - 1) / oscillator;
    const auto quotient = next / oscillator;
    next_sync_clock_known_ = next && tail <= std::numeric_limits<std::uint64_t>::max() - release_clock_ && quotient <=
        (std::numeric_limits<std::uint64_t>::max() - release_clock_ - tail) / scale;
    if (next_sync_clock_known_) next_sync_clock_ = release_clock_ + quotient * scale + tail;
}

bool SgbIcdGbSource::read(const std::uint16_t address,
                           const std::uint64_t master_clocks,
                           std::uint8_t& value) noexcept {
    synchronize(master_clocks);
    if (missing_address_ != 0) return false;
    switch (address) {
    case 0x6000: {
        // ICD status follows pixel scanlines, not the CPU's early LY=0 alias.
        const auto ly = last_ly_;
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
            // Cold reset rebuilds the APU. Restore destination-owned callback
            // bindings, never a pointer inherited from a saved machine.
            set_audio_sink(audio_sink_, audio_reset_sink_, audio_context_);
            bind_lcd_sink();
            pending_lcd_.fill({}); pending_lcd_head_ = pending_lcd_count_ = 0;
            lcd_target_ = lcd_clock_bias_ = 0;
            audio_samples_ = 0;
            audio_gap_cycles_ = 0;
            if (audio_sink_) {
                if (audio_reset_sink_) audio_reset_sink_(audio_context_, master_clocks);
            }
            gb_->bus().install_boot_rom(boot_image_);
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
            row_complete_.fill(false);
            for (auto& row : rows_) row.fill(0);
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
        next_sync_clock_known_ = false;
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
