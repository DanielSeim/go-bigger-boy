#include "gameboy/sgb_host.hpp"
#include "save_state_format.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace gameboy {
struct SgbHost::Impl {
    SgbProgramRom rom;
    SnesApuAudioEngine apu;
    SgbIcdGbSource icd;
    SnesHostCpu cpu;
    std::unique_ptr<SgbAudioMixer> mixer;
    SnesApuAudioEngine::DspWriteObserver diagnostic_observer{};
    SnesHostCpu::ApuPortObserver port_observer{};
    void* port_context{};
    void* diagnostic_context{};
    bool combined;
    unsigned apu_hz;
    std::uint64_t maximum_step_start{}; // Derived from immutable oscillator.
    bool direct_dsp_clock_enabled{true}; // Diagnostic binding, never saved.
    bool dsp_phase_dispatch_enabled{true}; // Execution choice, never saved.
    std::array<StereoSample, buffer_capacity> pcm{};
    std::size_t head{}, count{};
    std::uint64_t produced{};
    Status status{Status::ready};
    SnesHostCpu::StepResult fault{};
    explicit Impl(const SgbHostConfig& config)
        : rom(config.program_rom), icd(config.game_rom, config.gb_boot_rom, config.model),
          cpu(rom, apu.bus(), &apu.cpu_),
          mixer(config.combined_audio ? std::make_unique<SgbAudioMixer>(
              config.output_hz,config.gb_gain_q15,config.snes_gain_q15) : nullptr),
          combined(config.combined_audio), apu_hz(config.apu_clock_hz) {
        apu.install_ipl(config.spc_ipl);
        if (!config.battery_ram.empty()) icd.import_battery_ram(config.battery_ram);
        icd.set_native_gb_input(true);
        icd.set_input_events(config.input_events);
        icd.set_audio_enabled(combined);
        if (combined) icd.set_audio_sink([](void* context, std::uint64_t clock,
                                           std::int16_t left, std::int16_t right) noexcept {
            auto& self = *static_cast<Impl*>(context);
            if (!self.mixer->push(SgbAudioMixer::Source::gb,clock,{left,right}))
                self.status = Status::host_fault;
        }, [](void* context, std::uint64_t clock) noexcept {
            auto& self = *static_cast<Impl*>(context);
            if (!self.mixer->reset_gb_at(clock)) self.status = Status::host_fault;
        }, this);
        cpu.set_icd_source(&icd);
        cpu.set_ppu_dma_timing_enabled(true);
        cpu.set_host_bus_timing_enabled(true);
        cpu.set_fractional_apu_sync_enabled(true);
        if (!cpu.set_apu_clock_hz(config.apu_clock_hz))
            throw std::invalid_argument("invalid SGB host APU clock");
        maximum_step_start = std::numeric_limits<std::uint64_t>::max() /
            (std::uint64_t(config.apu_clock_hz) * 2) - 5'000'000;
        cpu.set_apu_batch_driver([](void* context, std::uint64_t target) noexcept {
            auto& self = *static_cast<Impl*>(context);
            // Keep buffering bounded even for a long DMA instruction. Drain
            // at most 16 DSP samples at once. Each sample's clock follows
            // phase 27, not the end of this batch or the host instruction.
            const auto drain = [&](std::size_t pending) noexcept {
              while (pending--) {
                StereoSample sample;
                if (!self.apu.pop_sample(sample)) return false;
                ++self.produced;
                if (self.combined) {
                    const auto halves = (self.produced * 32 - 4) * 2;
                    const auto hz = std::uint64_t(self.apu_hz)*2;
                    const auto clock = halves/hz*SgbAudioMixer::master_hz +
                        (halves%hz*SgbAudioMixer::master_hz+hz-1)/hz;
                    if (!self.mixer->push(SgbAudioMixer::Source::snes,clock,sample)) return false;
                } else {
                    if (self.count == buffer_capacity) return false;
                    self.pcm[(self.head + self.count++) % buffer_capacity] = sample;
                }
              }
              return true;
            };
            auto current = self.apu.cpu().half_cycles();
            while (current < target) {
                const auto stop = current + std::min<std::uint64_t>(1024, target-current);
                while (current < stop) {
                    const auto pc = self.apu.cpu().registers().pc;
                    const auto cycles = current / 2;
                    const auto pending = self.apu.pending_samples();
                    if (!self.apu.clock_half()) {
                        // Scalar execution had drained all earlier samples,
                        // but leaves any output from the failing half unread.
                        const auto drained = drain(pending);
                        self.status = drained ? Status::apu_fault : Status::host_fault;
                        return SnesHostCpu::ApuBatchResult{false, pc, cycles};
                    }
                    // clock_half checks exactly one physical half advanced;
                    // no need to reload processor metadata for loop bounds.
                    ++current;
                }
                if (!drain(self.apu.pending_samples())) {
                    self.status = Status::host_fault;
                    return SnesHostCpu::ApuBatchResult{false, self.apu.cpu().registers().pc,
                                                      self.apu.cpu().cycles()};
                }
            }
            return SnesHostCpu::ApuBatchResult{true, self.apu.cpu().registers().pc,
                                              current / 2};
        }, this);
        // Retain the original scalar scheduler as an independent oracle.
        cpu.set_apu_half_driver([](void* context) noexcept {
            auto& self = *static_cast<Impl*>(context);
            if (!self.apu.clock_half()) { self.status = Status::apu_fault; return false; }
            StereoSample sample;
            while (self.apu.pop_sample(sample)) {
                ++self.produced;
                if (self.combined) {
                    const auto halves = self.apu.cpu().half_cycles();
                    const auto hz = std::uint64_t(self.apu_hz)*2;
                    const auto clock = halves/hz*SgbAudioMixer::master_hz +
                        (halves%hz*SgbAudioMixer::master_hz+hz-1)/hz;
                    if (!self.mixer->push(SgbAudioMixer::Source::snes,clock,sample)) {
                        self.status = Status::host_fault; return false;
                    }
                } else {
                    if (self.count == buffer_capacity) { self.status = Status::host_fault; return false; }
                    self.pcm[(self.head + self.count++) % buffer_capacity] = sample;
                }
            }
            return true;
        }, this);
    }
};

SgbHost::SgbHost(SgbHostConfig config) : config_(std::move(config)) {
    if (config_.model != HardwareModel::sgb && config_.model != HardwareModel::sgb2)
        throw std::invalid_argument("SGB host requires SGB1 or SGB2 model");
    if (config_.output_hz < 8000 || config_.output_hz > 48000 ||
        config_.gb_gain_q15 > 32768 || config_.snes_gain_q15 > 32768)
        throw std::invalid_argument("invalid SGB audio rate or Q15 gain");
    reset();
}
SgbHost::~SgbHost() = default;
void SgbHost::debug_set_dsp_write_observer(SnesApuAudioEngine::DspWriteObserver observer,
                                         void* context) noexcept {
    impl_->diagnostic_observer=observer; impl_->diagnostic_context=context;
    impl_->apu.debug_set_dsp_write_observer(observer, context);
}
void SgbHost::debug_set_apu_port_observer(SnesHostCpu::ApuPortObserver observer,
                                        void* context) noexcept {
    impl_->port_observer=observer; impl_->port_context=context;
    impl_->cpu.set_apu_port_observer(observer, context);
}
std::uint8_t SgbHost::debug_spc_ram_byte(std::uint16_t address) const noexcept {
    return impl_->apu.bus().dsp_read_ram(address);
}
std::uint8_t SgbHost::debug_dsp_register(std::uint8_t index) const noexcept {
    return impl_->apu.bus().dsp_register(index);
}
void SgbHost::debug_set_apu_batch_enabled(bool enabled) noexcept {
    impl_->cpu.debug_set_apu_batch_enabled(enabled);
}
void SgbHost::debug_set_spc_idle_tail_cache_enabled(bool enabled) noexcept {
    impl_->apu.cpu_.debug_set_idle_tail_cache_enabled(enabled);
}
void SgbHost::debug_set_direct_dsp_clock_enabled(bool enabled) noexcept {
    impl_->direct_dsp_clock_enabled=enabled;
    impl_->apu.debug_set_direct_dsp_clock_enabled(enabled);
}
void SgbHost::debug_set_dsp_phase_dispatch_enabled(bool enabled) noexcept {
    impl_->dsp_phase_dispatch_enabled=enabled;
    impl_->apu.debug_set_dsp_phase_dispatch_enabled(enabled);
}
void SgbHost::reset() {
    auto fresh = std::make_unique<Impl>(config_);
    fresh->direct_dsp_clock_enabled=impl_ ? impl_->direct_dsp_clock_enabled : true;
    fresh->apu.debug_set_direct_dsp_clock_enabled(fresh->direct_dsp_clock_enabled);
    fresh->dsp_phase_dispatch_enabled=impl_ ? impl_->dsp_phase_dispatch_enabled : true;
    fresh->apu.debug_set_dsp_phase_dispatch_enabled(fresh->dsp_phase_dispatch_enabled);
    if (impl_) {
        fresh->diagnostic_observer=impl_->diagnostic_observer;
        fresh->diagnostic_context=impl_->diagnostic_context;
        fresh->apu.debug_set_dsp_write_observer(fresh->diagnostic_observer,fresh->diagnostic_context);
        fresh->port_observer=impl_->port_observer;
        fresh->port_context=impl_->port_context;
        fresh->cpu.set_apu_port_observer(fresh->port_observer,fresh->port_context);
    }
    impl_.swap(fresh);
}
bool SgbHost::step() noexcept {
    auto& s = *impl_;
    if (s.status != Status::ready && s.status != Status::buffer_full) return false;
    if (buffer_capacity - pending_samples() <
            (s.combined ? combined_instruction_reserve : instruction_reserve)) {
        s.status = Status::buffer_full; return false;
    }
    // Guard the CPU's absolute-clock multiply against overflow. No wraparound
    // may replay a host/APU access; the caller must reset at this extreme limit.
    if (s.cpu.timing().clocks() > s.maximum_step_start) {
        s.status = Status::host_fault; return false;
    }
    s.status = Status::ready;
    s.fault = s.cpu.step();
    s.cpu.clear_apu_writes(); // Bounded diagnostics must not become execution state.
    if (s.fault.error != SnesHostCpu::Error::none) {
        if (s.status == Status::ready)
            s.status = s.icd.missing_address() ? Status::icd_fault : Status::host_fault;
        return false;
    }
    s.icd.advance_to(s.cpu.timing().clocks());
    if (s.icd.missing_address()) { s.status = Status::icd_fault; return false; }
    if (s.status != Status::ready) return false;
    if (s.combined && !s.mixer->advance_to(s.cpu.timing().clocks())) {
        s.status = Status::host_fault; return false;
    }
    return true;
}
std::uint64_t SgbHost::run_until(std::uint64_t target, std::uint64_t budget) noexcept {
    std::uint64_t completed{};
    while (completed < budget && impl_->cpu.timing().clocks() < target && step()) ++completed;
    return completed;
}
bool SgbHost::pop_sample(StereoSample& sample) noexcept {
    auto& s = *impl_;
    if (s.combined) {
        if (!s.mixer->pop_sample(sample)) return false;
        if (s.status == Status::buffer_full && buffer_capacity-pending_samples() >= combined_instruction_reserve)
            s.status = Status::ready;
        return true;
    }
    if (!s.count) return false;
    sample = s.pcm[s.head]; s.pcm[s.head] = {};
    s.head = (s.head + 1) % buffer_capacity; --s.count;
    if (s.status == Status::buffer_full && buffer_capacity-s.count >= instruction_reserve)
        s.status = Status::ready;
    return true;
}
std::size_t SgbHost::pending_samples() const noexcept {
    return impl_->combined ? impl_->mixer->pending_samples() : impl_->count;
}
SgbHost::Status SgbHost::status() const noexcept { return impl_->status; }
const SnesHostCpu& SgbHost::cpu() const noexcept { return impl_->cpu; }
const SgbIcdGbSource& SgbHost::icd() const noexcept { return impl_->icd; }
void SgbHost::set_button(Button button, bool pressed) noexcept {
    impl_->icd.set_live_button(button, pressed);
}
void SgbHost::import_battery_ram(const std::vector<std::uint8_t>& bytes) {
    impl_->icd.import_battery_ram(bytes);
}
std::uint64_t SgbHost::apu_half_clocks() const noexcept { return impl_->apu.cpu().half_cycles(); }
std::uint64_t SgbHost::samples_produced() const noexcept {
    return impl_->combined ? impl_->mixer->samples_produced() : impl_->produced;
}
std::uint64_t SgbHost::snes_samples_produced() const noexcept { return impl_->produced; }
std::uint64_t SgbHost::gb_samples_captured() const noexcept { return impl_->icd.audio_samples_captured(); }
std::uint64_t SgbHost::clipped_samples() const noexcept { return impl_->combined ? impl_->mixer->clipped_samples() : 0; }
unsigned SgbHost::sample_rate() const noexcept { return impl_->combined ? impl_->mixer->sample_rate() : impl_->apu_hz/32; }
SnesHostCpu::StepResult SgbHost::fault() const noexcept { return impl_->fault; }

// Explicit scalar encoding, never object bytes/padding/pointers. Byte fields
// use u8, all other integers use u64 (also for size_t on 32-bit frontends).
// Parsing builds a fully wired candidate, then swaps it into the destination.
class SgbHostStateCodec {
    static constexpr std::size_t limit = 16 * 1024 * 1024;
    struct Writer {
        save_state_format::Writer out;
        template<class... T> void operator()(T&... fields) { (value(fields), ...); }
        template<class T> void value(T& x) {
            if constexpr (std::is_enum_v<T>) out.u8(static_cast<std::uint8_t>(x));
            else if constexpr (sizeof(T) == 1) out.u8(static_cast<std::uint8_t>(x));
            else {
                using U = std::make_unsigned_t<T>; U bits; std::memcpy(&bits, &x, sizeof(T));
                out.u64(static_cast<std::uint64_t>(bits));
            }
        }
        template<class T, std::size_t N> void value(std::array<T,N>& x) { for (auto& v:x) value(v); }
        template<class T> void value(std::vector<T>& x) { out.u64(x.size()); for(auto& v:x) value(v); }
        template<class T> void value(std::deque<T>& x) { out.u64(x.size()); for(auto& v:x) value(v); }
        void value(SnesHostCpu::ApuWrite& x) { (*this)(x.step,x.port,x.value); }
        void value(SgbIcdGbSource::InputEvent& x) { (*this)(x.frame,x.mask); }
        void value(SgbIcdGbSource::LcdEvent& x) { (*this)(x.clock,x.x,x.y,x.pixel); }
        void value(SgbHost::StereoSample& x) { (*this)(x.left,x.right); }
        void value(SgbAudioMixer::Event& x) { (*this)(x.clock,x.sample); }
        void value(SgbAudioMixer::Stream& x) { (*this)(x.events,x.head,x.count,x.last_clock,x.held); }
    };
    struct Reader {
        save_state_format::Reader in;
        explicit Reader(const std::vector<std::uint8_t>& state) : in(state,9,state.size()-17) {}
        template<class... T> void operator()(T&... fields) { (value(fields), ...); }
        template<class T> void value(T& x) {
            if constexpr (std::is_same_v<T,bool>) x = in.boolean();
            else if constexpr (std::is_enum_v<T> || sizeof(T) == 1) x = static_cast<T>(in.u8());
            else {
                using U = std::make_unsigned_t<T>; const auto raw = in.u64();
                if (raw > std::numeric_limits<U>::max()) throw SaveStateError("SGB scalar overflow");
                const auto bits = static_cast<U>(raw); std::memcpy(&x, &bits, sizeof(T));
            }
        }
        template<class T, std::size_t N> void value(std::array<T,N>& x) { for(auto& v:x) value(v); }
        template<class T> void value(std::vector<T>& x) {
            const auto n = in.u64();
            const auto bound = std::is_same_v<T,std::uint8_t> ? limit : 16384;
            if(n > bound || n > in.remaining()) throw SaveStateError("SGB vector overflow");
            x.resize(static_cast<std::size_t>(n)); for(auto& v:x) value(v);
        }
        template<class T> void value(std::deque<T>& x) {
            const auto n=in.u64(); if(n>32) throw SaveStateError("SGB packet queue overflow");
            x.resize(static_cast<std::size_t>(n)); for(auto& v:x) value(v);
        }
        void value(SnesHostCpu::ApuWrite& x) { (*this)(x.step,x.port,x.value); }
        void value(SgbIcdGbSource::InputEvent& x) { (*this)(x.frame,x.mask); }
        void value(SgbIcdGbSource::LcdEvent& x) { (*this)(x.clock,x.x,x.y,x.pixel); }
        void value(SgbHost::StereoSample& x) { (*this)(x.left,x.right); }
        void value(SgbAudioMixer::Event& x) { (*this)(x.clock,x.sample); }
        void value(SgbAudioMixer::Stream& x) { (*this)(x.events,x.head,x.count,x.last_clock,x.held); }
    };
    template<class C> static void cpu(C& c, SnesHostCpu& s) {
        if constexpr (std::is_same_v<C,Reader>) s.irq_cache_valid_ = false;
        c(s.cycle_apu_sync_,s.fractional_apu_sync_,s.ppu_dma_timing_,s.host_bus_timing_,
          s.pending_ppu_dma_,s.spc_cycles_,s.apu_clock_hz_,s.r_.pc,s.r_.a,s.r_.x,s.r_.y,
          s.r_.s,s.r_.d,s.r_.pb,s.r_.db,s.r_.p,s.r_.e,s.wram_,s.dma_registers_,
          s.dma_destination_counts_,s.last_wram_dma_registers_,s.wram_port_address_,
          s.last_wram_dma_target_,s.dma_start_count_,s.last_dma_mask_,s.multiply_a_,
          s.dividend_,s.quotient_,s.product_or_remainder_,s.pending_quotient_,
          s.pending_product_or_remainder_,s.cpu_cycles_,s.math_ready_cycle_,s.math_result_valid_,
          s.quotient_valid_,s.math_pending_,s.pending_division_,s.apu_writes_,s.apu_write_count_,s.steps_);
        auto& t=s.timing_;
        c(t.clocks_,t.line_,t.horizontal_clock_,t.field_,t.overscan_,t.latched_,t.latch_enable_,
          t.refresh_done_,t.nmi_latched_,t.autojoy_enabled_,t.frames_,t.frame_start_clocks_);
        c(s.bus_accesses_,s.fast_rom_,s.interrupt_enable_,s.irq_h_target_,s.irq_v_target_,
          s.last_irq_clock_,s.irq_latched_,s.irq_defer_after_cli_,s.irq_entries_,s.open_bus_,
          s.latched_h_,s.latched_v_,s.h_counter_high_,s.v_counter_high_,s.nmi_was_enabled_,
          s.branch_taken_,s.branch_crossed_,s.indexed_extra_,s.error_,s.error_address_);
    }
    template<class C> static void icd(C& c, SgbIcdGbSource& s) {
        c(s.boot_image_,s.release_clock_,s.master_snapshot_,s.gb_cycles_,s.packets_completed_,
          s.sound_commands_,s.packets_delivered_,s.sound_packets_delivered_,s.audible_sound_packets_delivered_,
          s.first_sound_packet_,s.last_delivered_sound_packet_,s.first_audible_sound_packet_,
          s.audible_sound_commands_,s.first_audible_frame_,s.completed_frames_,s.input_events_applied_,
          s.input_events_,s.next_input_event_,s.held_buttons_);
        auto& f=s.frame_input_; c(f.size_,f.next_,f.held_);
        for(auto& e:f.events_) c(e.frame,e.mask);
        c(s.native_gb_input_,s.transfer_commands_,s.missing_address_,s.control_writes_,s.last_control_,
          s.divider_,s.model_,s.released_,s.boot_reported_,s.pulse_armed_,s.receiving_,s.packet_pending_,
          s.audible_sound_substitution_,s.bit_count_,s.continuation_packets_,s.last_ly_,s.selected_row_,
          s.row_stream_offset_,s.rows_,s.row_valid_,s.row_complete_,s.building_,s.latched_,s.queued_,
          s.audio_samples_,s.audio_captured_,s.audio_gap_cycles_,
          s.pending_lcd_,s.pending_lcd_head_,s.pending_lcd_count_);
    }
    static std::uint64_t hash(const std::uint8_t* data, std::size_t n) {
        std::uint64_t h=14695981039346656037ULL;
        for(std::size_t i=0;i<n;++i) { h^=data[i]; h*=1099511628211ULL; } return h;
    }
    static std::uint64_t identity(const SgbHostConfig& cfg) {
        Writer w; auto& c=const_cast<SgbHostConfig&>(cfg);
        w(c.program_rom,c.game_rom,c.battery_ram,c.gb_boot_rom,c.spc_ipl,c.model,c.apu_clock_hz,c.input_events,
          c.combined_audio,c.output_hz,c.gb_gain_q15,c.snes_gain_q15);
        return hash(w.out.data().data(),w.out.data().size());
    }
    template<class C> static void components(C& c, SgbHost::Impl& s) {
        c(s.pcm,s.head,s.count,s.produced,s.status,s.fault.error,s.fault.opcode,
          s.fault.bank,s.fault.pc,s.fault.address);
        cpu(c,s.cpu); icd(c,s.icd);
        // The application GB snapshot does not retain the live indexed LCD
        // image used by ICD tile-row reads. Preserve it in this host container.
        c(*s.icd.gb_->bus().ppu_.sgb_screen_buffer_);
        if (s.combined) {
            auto& m = *s.mixer;
            c(m.output_hz_,m.gb_gain_q15_,m.snes_gain_q15_,m.streams_,m.pcm_,m.head_,m.count_,
              m.time_,m.produced_,m.clipped_,m.left_area_,m.right_area_);
        }
    }
    static void validate(const SgbHostConfig& cfg, SgbHost::Impl& s) {
        auto& c=s.cpu; auto& i=s.icd; auto& t=c.timing_; auto& f=i.frame_input_;
        if(s.head>=SgbHost::buffer_capacity || s.count>SgbHost::buffer_capacity || s.produced<s.count ||
           s.status>Status::icd_fault || s.fault.error>SnesHostCpu::Error::trace_overflow ||
           c.error_>SnesHostCpu::Error::trace_overflow || c.apu_writes_.size()!=16384 || c.apu_write_count_!=0 ||
           !c.cycle_apu_sync_ || !c.fractional_apu_sync_ || !c.ppu_dma_timing_ || !c.host_bus_timing_ ||
           c.apu_clock_hz_!=cfg.apu_clock_hz ||
           c.wram_port_address_>=0x20000 || c.last_wram_dma_target_>=0x20000 ||
           t.line_>=262 || t.horizontal_clock_>=t.line_length() || t.frame_start_clocks_>t.clocks_ ||
           c.steps_>t.clocks_/6 || c.cpu_cycles_>t.clocks_/6 ||
           c.math_ready_cycle_>c.cpu_cycles_+16 || c.error_address_>0xffffff || s.fault.address>0xffffff ||
           (c.r_.e && ((c.r_.p&0x30)!=0x30 || (c.r_.s&0xff00)!=0x100)) ||
           ((c.r_.e || (c.r_.p&0x10)) && (c.r_.x>255 || c.r_.y>255)) ||
           c.last_irq_clock_>t.clocks_ || c.irq_h_target_>511 || c.irq_v_target_>511 ||
           !i.native_gb_input_ || i.audible_sound_substitution_ || i.model_!=cfg.model || i.boot_image_!=cfg.gb_boot_rom ||
           (i.divider_!=4 && i.divider_!=5 && i.divider_!=7 && i.divider_!=9) ||
           i.release_clock_>t.clocks_ || i.master_snapshot_>t.clocks_ || i.bit_count_>128 ||
           i.continuation_packets_>6 || i.last_ly_>153 || i.selected_row_>3 || i.row_stream_offset_>511 ||
           i.pending_lcd_head_>=i.pending_lcd_.size() || i.pending_lcd_count_>i.pending_lcd_.size() ||
           i.next_input_event_>i.input_events_.size() || i.input_events_.size()>1024 ||
           f.size_!=i.input_events_.size() || f.next_>f.size_ || f.size_>1024 ||
           f.next_!=i.next_input_event_ || f.held_!=i.held_buttons_ ||
           i.gb_cycles_>t.clocks_/4+32 ||
           i.packets_delivered_>i.packets_completed_ || i.sound_packets_delivered_>i.packets_delivered_ ||
           i.audible_sound_packets_delivered_>i.sound_packets_delivered_)
            throw SaveStateError("invalid SGB host state");
        const auto lcd_target = i.master_snapshot_ < i.release_clock_ ? 0 :
            sgb_icd_target_gb_cycles(i.master_snapshot_ - i.release_clock_, i.divider_, i.model_);
        std::uint64_t previous_clock = lcd_target;
        for (unsigned bank = 0; bank < 4; ++bank)
            if (i.row_complete_[bank] && !i.row_valid_[bank])
                throw SaveStateError("complete LCD bank lacks initialized data");
        for (unsigned n = 0; n < i.pending_lcd_count_; ++n) {
            const auto& e = i.pending_lcd_[(i.pending_lcd_head_ + n) % i.pending_lcd_.size()];
            if (e.clock <= lcd_target || e.clock < previous_clock || e.clock > i.gb_cycles_ || e.x > 161 || e.y > 153 ||
                (e.x < 160 && (e.y >= 144 || e.pixel > 3)) ||
                (e.x == 160 && e.pixel != 0) || (e.x == 161 && (e.y != 0 || e.pixel > 1)))
                throw SaveStateError("invalid pending LCD event");
            previous_clock = e.clock;
        }
        SgbFrameInput input;
        for(std::size_t n=0;n<i.input_events_.size();++n) {
            const auto& e=i.input_events_[n];
            if(!input.add(e.frame,e.mask) || f.events_[n].frame!=e.frame || f.events_[n].mask!=e.mask)
                throw SaveStateError("invalid SGB input state");
        }
        if (cfg.combined_audio) {
            const auto& m = *s.mixer;
            if (!m.validate() || m.output_hz_ != cfg.output_hz || m.gb_gain_q15_ != cfg.gb_gain_q15 ||
                m.snes_gain_q15_ != cfg.snes_gain_q15 || i.audio_samples_ > i.audio_captured_ ||
                i.audio_gap_cycles_ > i.gb_cycles_ || i.audio_samples_ > i.gb_cycles_ ||
                i.audio_captured_ > t.clocks_ || s.count != 0 ||
                ((s.status == Status::ready || s.status == Status::buffer_full) && m.time_ != t.clocks_*cfg.output_hz))
                throw SaveStateError("invalid combined SGB audio state");
        } else if (i.audio_samples_ || i.audio_captured_ || i.audio_gap_cycles_)
            throw SaveStateError("unexpected GB audio capture");
        // At successful instruction boundaries the APU is caught up exactly.
        const auto half_hz=std::uint64_t(cfg.apu_clock_hz)*2;
        if(t.clocks_>std::numeric_limits<std::uint64_t>::max()/half_hz)
            throw SaveStateError("SGB clock overflow");
        const auto expected=t.clocks_*half_hz/21477273;
        if((s.status==Status::ready || s.status==Status::buffer_full) &&
           (s.apu.cpu().half_cycles()!=expected || s.apu.pending_samples()!=0 ||
            c.spc_cycles_!=s.apu.cpu().cycles() ||
            s.produced!=(s.apu.cpu().cycles()+4)/32 || i.missing_address_!=0))
            throw SaveStateError("SGB clock or queue disagreement");
    }
    using Status=SgbHost::Status;
public:
    static std::vector<std::uint8_t> save(const SgbHost& host) {
        Writer w; const std::array<std::uint8_t,8> signature{'G','B','B','S','H','O','S','T'};
        w.out.bytes(signature.data(),signature.size()); w.out.u8(3);
        auto id=identity(host.config_); w(id);
        auto& s=*host.impl_; components(w,s);
        auto apu=s.apu.save_state(), gb=s.icd.gb_->save_state(); w(apu,gb);
        auto digest=hash(w.out.data().data(),w.out.data().size()); w(digest);
        if(w.out.data().size()>limit) throw SaveStateError("SGB host state too large");
        return w.out.take();
    }
    static bool load(SgbHost& host,const std::vector<std::uint8_t>& state) noexcept {
        try {
            if(state.size()<25 || state.size()>limit ||
               std::memcmp(state.data(),"GBBSHOST",8)!=0 || state[8]!=3) return false;
            save_state_format::Reader tail(state,state.size()-8,8);
            if(tail.u64()!=hash(state.data(),state.size()-8)) return false;
            Reader r(state); std::uint64_t id{}; r(id);
            if(id!=identity(host.config_)) return false;
            auto candidate=std::make_unique<SgbHost::Impl>(host.config_);
            candidate->direct_dsp_clock_enabled=host.impl_->direct_dsp_clock_enabled;
            candidate->apu.debug_set_direct_dsp_clock_enabled(candidate->direct_dsp_clock_enabled);
            candidate->dsp_phase_dispatch_enabled=host.impl_->dsp_phase_dispatch_enabled;
            candidate->apu.debug_set_dsp_phase_dispatch_enabled(candidate->dsp_phase_dispatch_enabled);
            candidate->diagnostic_observer=host.impl_->diagnostic_observer;
            candidate->diagnostic_context=host.impl_->diagnostic_context;
            candidate->apu.debug_set_dsp_write_observer(candidate->diagnostic_observer,candidate->diagnostic_context);
            candidate->port_observer=host.impl_->port_observer;
            candidate->port_context=host.impl_->port_context;
            candidate->cpu.set_apu_port_observer(candidate->port_observer,candidate->port_context);
            components(r,*candidate);
            std::vector<std::uint8_t> apu,gb; r(apu,gb); r.in.finish();
            if(!candidate->apu.load_state(apu)) return false;
            candidate->icd.gb_->load_state(gb);
            candidate->icd.bind_lcd_sink();
            (void)candidate->icd.gb_->bus().debug_take_io_trace();
            validate(host.config_,*candidate);
            if (candidate->combined) candidate->mixer->refresh_cache();
            host.impl_.swap(candidate); return true;
        } catch(...) { return false; }
    }
};
std::vector<std::uint8_t> SgbHost::save_state() const { return SgbHostStateCodec::save(*this); }
bool SgbHost::load_state(const std::vector<std::uint8_t>& state) noexcept { return SgbHostStateCodec::load(*this,state); }
} // namespace gameboy
