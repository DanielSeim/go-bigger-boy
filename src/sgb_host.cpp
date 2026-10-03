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
    std::array<StereoSample, buffer_capacity> pcm{};
    std::size_t head{}, count{};
    std::uint64_t produced{};
    Status status{Status::ready};
    SnesHostCpu::StepResult fault{};
    explicit Impl(const SgbHostConfig& config)
        : rom(config.program_rom), icd(config.game_rom, config.gb_boot_rom, config.model),
          cpu(rom, apu.bus(), &apu.cpu_) {
        apu.install_ipl(config.spc_ipl);
        if (!config.battery_ram.empty()) icd.import_battery_ram(config.battery_ram);
        icd.set_native_gb_input(true);
        icd.set_input_events(config.input_events);
        // Keep GB channel clocks/registers running, but don't accumulate a
        // second, unused frontend audio queue. Mixing is a separate future step.
        icd.set_audio_enabled(false);
        cpu.set_icd_source(&icd);
        cpu.set_ppu_dma_timing_enabled(true);
        cpu.set_host_bus_timing_enabled(true);
        cpu.set_fractional_apu_sync_enabled(true);
        if (!cpu.set_apu_clock_hz(config.apu_clock_hz))
            throw std::invalid_argument("invalid SGB host APU clock");
        cpu.set_apu_half_driver([](void* context) noexcept {
            auto& self = *static_cast<Impl*>(context);
            if (!self.apu.clock_half()) { self.status = Status::apu_fault; return false; }
            StereoSample sample;
            while (self.apu.pop_sample(sample)) {
                if (self.count == buffer_capacity) {
                    self.status = Status::host_fault; return false;
                }
                self.pcm[(self.head + self.count++) % buffer_capacity] = sample;
                ++self.produced;
            }
            return true;
        }, this);
    }
};

SgbHost::SgbHost(SgbHostConfig config) : config_(std::move(config)) {
    if (config_.model != HardwareModel::sgb && config_.model != HardwareModel::sgb2)
        throw std::invalid_argument("SGB host requires SGB1 or SGB2 model");
    reset();
}
SgbHost::~SgbHost() = default;
void SgbHost::reset() { auto fresh = std::make_unique<Impl>(config_); impl_.swap(fresh); }
bool SgbHost::step() noexcept {
    auto& s = *impl_;
    if (s.status != Status::ready && s.status != Status::buffer_full) return false;
    if (buffer_capacity - s.count < instruction_reserve) {
        s.status = Status::buffer_full; return false;
    }
    // Guard the CPU's absolute-clock multiply against overflow. No wraparound
    // may replay a host/APU access; the caller must reset at this extreme limit.
    constexpr std::uint64_t max_instruction_clocks = 5'000'000;
    if (s.cpu.timing().clocks() > std::numeric_limits<std::uint64_t>::max() /
            (std::uint64_t(config_.apu_clock_hz) * 2) - max_instruction_clocks) {
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
    return true;
}
std::uint64_t SgbHost::run_until(std::uint64_t target, std::uint64_t budget) noexcept {
    std::uint64_t completed{};
    while (completed < budget && impl_->cpu.timing().clocks() < target && step()) ++completed;
    return completed;
}
bool SgbHost::pop_sample(StereoSample& sample) noexcept {
    auto& s = *impl_;
    if (!s.count) return false;
    sample = s.pcm[s.head]; s.pcm[s.head] = {};
    s.head = (s.head + 1) % buffer_capacity; --s.count;
    if (s.status == Status::buffer_full && buffer_capacity-s.count >= instruction_reserve)
        s.status = Status::ready;
    return true;
}
std::size_t SgbHost::pending_samples() const noexcept { return impl_->count; }
SgbHost::Status SgbHost::status() const noexcept { return impl_->status; }
const SnesHostCpu& SgbHost::cpu() const noexcept { return impl_->cpu; }
const SgbIcdGbSource& SgbHost::icd() const noexcept { return impl_->icd; }
std::uint64_t SgbHost::apu_half_clocks() const noexcept { return impl_->apu.cpu().half_cycles(); }
std::uint64_t SgbHost::samples_produced() const noexcept { return impl_->produced; }
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
        void value(SgbHost::StereoSample& x) { (*this)(x.left,x.right); }
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
        void value(SgbHost::StereoSample& x) { (*this)(x.left,x.right); }
    };
    template<class C> static void cpu(C& c, SnesHostCpu& s) {
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
          s.row_stream_offset_,s.rows_,s.row_valid_,s.building_,s.latched_,s.queued_);
    }
    static std::uint64_t hash(const std::uint8_t* data, std::size_t n) {
        std::uint64_t h=14695981039346656037ULL;
        for(std::size_t i=0;i<n;++i) { h^=data[i]; h*=1099511628211ULL; } return h;
    }
    static std::uint64_t identity(const SgbHostConfig& cfg) {
        Writer w; auto& c=const_cast<SgbHostConfig&>(cfg);
        w(c.program_rom,c.game_rom,c.battery_ram,c.gb_boot_rom,c.spc_ipl,c.model,c.apu_clock_hz,c.input_events);
        return hash(w.out.data().data(),w.out.data().size());
    }
    template<class C> static void components(C& c, SgbHost::Impl& s) {
        c(s.pcm,s.head,s.count,s.produced,s.status,s.fault.error,s.fault.opcode,
          s.fault.bank,s.fault.pc,s.fault.address);
        cpu(c,s.cpu); icd(c,s.icd);
        // The application GB snapshot does not retain the live indexed LCD
        // image used by ICD tile-row reads. Preserve it in this host container.
        c(*s.icd.gb_->bus().ppu_.sgb_screen_buffer_);
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
           i.next_input_event_>i.input_events_.size() || i.input_events_.size()>1024 ||
           f.size_!=i.input_events_.size() || f.next_>f.size_ || f.size_>1024 ||
           f.next_!=i.next_input_event_ || f.held_!=i.held_buttons_ ||
           i.gb_cycles_>t.clocks_/4+32 ||
           i.packets_delivered_>i.packets_completed_ || i.sound_packets_delivered_>i.packets_delivered_ ||
           i.audible_sound_packets_delivered_>i.sound_packets_delivered_)
            throw SaveStateError("invalid SGB host state");
        SgbFrameInput input;
        for(std::size_t n=0;n<i.input_events_.size();++n) {
            const auto& e=i.input_events_[n];
            if(!input.add(e.frame,e.mask) || f.events_[n].frame!=e.frame || f.events_[n].mask!=e.mask)
                throw SaveStateError("invalid SGB input state");
        }
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
        w.out.bytes(signature.data(),signature.size()); w.out.u8(1);
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
               std::memcmp(state.data(),"GBBSHOST",8)!=0 || state[8]!=1) return false;
            save_state_format::Reader tail(state,state.size()-8,8);
            if(tail.u64()!=hash(state.data(),state.size()-8)) return false;
            Reader r(state); std::uint64_t id{}; r(id);
            if(id!=identity(host.config_)) return false;
            auto candidate=std::make_unique<SgbHost::Impl>(host.config_);
            components(r,*candidate);
            std::vector<std::uint8_t> apu,gb; r(apu,gb); r.in.finish();
            if(!candidate->apu.load_state(apu)) return false;
            candidate->icd.gb_->load_state(gb);
            (void)candidate->icd.gb_->bus().debug_take_io_trace();
            validate(host.config_,*candidate);
            host.impl_.swap(candidate); return true;
        } catch(...) { return false; }
    }
};
std::vector<std::uint8_t> SgbHost::save_state() const { return SgbHostStateCodec::save(*this); }
bool SgbHost::load_state(const std::vector<std::uint8_t>& state) noexcept { return SgbHostStateCodec::load(*this,state); }
} // namespace gameboy
