#include "gameboy/snes_dsp_audio_engine.hpp"
#include "gameboy/snes_audio_host.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace gameboy {
namespace {

// Explicit scalar encoding rather than copying compiler object layouts.
struct Writer {
    std::vector<std::uint8_t> bytes;
    template<class T> void operator()(const T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            bytes.push_back(value ? 1 : 0);
        } else if constexpr (std::is_enum_v<T>) {
            // All engine enums have at most eight bits of defined state.
            bytes.push_back(static_cast<std::uint8_t>(value));
        } else {
            static_assert(std::is_integral_v<T>);
            using U = std::make_unsigned_t<T>;
            const auto bits = static_cast<U>(value);
            for (unsigned i = 0; i < sizeof(T); ++i)
                bytes.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
        }
    }
    template<class T, std::size_t N> void operator()(const std::array<T, N>& a) {
        if constexpr (std::is_same_v<T, std::uint8_t>)
            bytes.insert(bytes.end(), a.begin(), a.end());
        else for (const auto& value : a) (*this)(value);
    }
};

struct Reader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t position{};
    bool valid{true};
    template<class T> void operator()(T& value) noexcept {
        if constexpr (std::is_same_v<T, bool> || std::is_enum_v<T>) {
            if (position == bytes.size()) { valid = false; return; }
            const auto raw = bytes[position++];
            if constexpr (std::is_same_v<T, bool>) {
                if (raw > 1) valid = false;
            }
            value = static_cast<T>(raw);
        } else {
            static_assert(std::is_integral_v<T>);
            if (sizeof(T) > bytes.size() - position) { valid = false; return; }
            using U = std::make_unsigned_t<T>;
            U raw{};
            for (unsigned i = 0; i < sizeof(T); ++i)
                raw |= static_cast<U>(static_cast<U>(bytes[position++]) << (i * 8));
            if constexpr (std::is_signed_v<T>) {
                // Avoid implementation-defined unsigned-to-signed conversion.
                value = raw <= static_cast<U>(std::numeric_limits<T>::max())
                    ? static_cast<T>(raw)
                    : static_cast<T>(-1 - static_cast<T>(static_cast<U>(~raw)));
            } else value = raw;
        }
    }
    template<class T, std::size_t N> void operator()(std::array<T, N>& a) noexcept {
        if constexpr (std::is_same_v<T, std::uint8_t>) {
            if (N > bytes.size() - position) { valid = false; return; }
            std::copy_n(bytes.data() + position, N, a.data());
            position += N;
        } else for (auto& value : a) (*this)(value);
    }
};

constexpr std::array<std::uint8_t, 8> signature{'G', 'B', 'B', 'S', 'D', 'S', 'P', 1};

} // namespace

// One codec can see the component internals, without exposing mutable DSP
// state to callers. Field order below defines version 1, not sizeof(class).
class SnesDspStateCodec final {
    template<class IO, class Sample> static void sample(IO& io, Sample& s) {
        io(s.left); io(s.right);
    }
    template<class IO, class Engine> static void fields(IO& io, Engine& e) {
        auto& b = e.bus_;
        io(b.ram_); io(b.dsp_); io(b.host_to_spc_); io(b.spc_to_host_);
        io(b.timer_target_); io(b.timer_stage2_); io(b.timer_output_);
        // 'unsigned' is not a wire type; encode timer phases as fixed uint32.
        for (auto& phase : b.timer_phase_) {
            std::uint32_t fixed = phase; io(fixed);
            if constexpr (!std::is_const_v<Engine>) phase = fixed;
        }
        io(b.timer_enabled_); io(b.ipl_); io(b.has_ipl_);
        io(b.ipl_enabled_); io(b.dsp_address_);
        auto& r = e.renderer_;
        for (auto& voice : r.voices_) {
            io(voice.stream.decoder_.previous_); io(voice.stream.decoder_.before_previous_);
            io(voice.stream.block_address_); io(voice.stream.group_index_);
            io(voice.ring.samples_); io(voice.ring.phase_); io(voice.ring.write_position_);
            io(voice.envelope.envelope_); io(voice.envelope.preclamp_); io(voice.envelope.phase_);
            io(voice.sequence.stage_); io(voice.started);
        }
        io(r.rates_.counter_);
        io(r.keys_.pending_kon_); io(r.keys_.koff_register_); io(r.keys_.sampled_koff_);
        io(r.keys_.soft_reset_); io(r.keys_.poll_next_); io(r.keys_.timed_new_kon_);
        io(r.keys_.timed_polled_kon_); io(r.keys_.timed_sampled_koff_); io(r.keys_.timed_poll_next_);
        io(r.ends_.endx_); io(r.timed_endx_visible_); io(r.timed_pmon_); io(r.timed_non_);
        io(r.timed_eon_); io(r.timed_dir_); io(r.timed_feedback_); io(r.timed_fir_);
        io(r.timed_echo_enabled_); io(r.timed_mode_);
        io(r.current_keys_.key_on); io(r.current_keys_.key_off); io(r.current_keys_.soft_reset);
        io(r.voice_output16_); io(r.live_readback_); io(r.live_envx_);
        io(r.live_loop_event_); io(r.live_kon_event_); io(r.live_envx_buffer_);
        io(r.live_outx_buffer_); io(r.live_endx_buffer_); sample(io, r.live_echo_input_);
        for (auto& registers : r.timed_voice_registers_) {
            io(registers.directory); io(registers.source); io(registers.pitch_low);
            io(registers.pitch_high); io(registers.adsr0);
        }
        sample(io, r.pending_mix_); sample(io, r.pending_echo_send_);
        sample(io, r.timed_echo_writeback_); io(r.timed_echo_address_); io(r.timed_echo_write_pending_);
        for (auto& s : r.echo_history_) sample(io, s);
        io(r.echo_offset_); io(r.echo_length_); io(r.echo_history_position_); io(r.echo_esa_); io(r.noise_);
        io(e.clock_.count_); io(e.clock_.left_); io(e.clock_.right_);
        io(e.clock_.echo_left_); io(e.clock_.echo_right_);
        io(e.head_); io(e.size_);
        for (auto& s : e.buffer_) sample(io, s);
    }
    static bool valid(const SnesDspAudioEngine& e) noexcept {
        const auto& b = e.bus_;
        for (unsigned i = 0; i < 3; ++i)
            if (b.timer_phase_[i] >= (i == 2 ? 16U : 128U) || b.timer_output_[i] > 15) return false;
        if (b.timer_enabled_ > 7 || e.head_ >= e.buffer_capacity || e.size_ > e.buffer_capacity) return false;
        const auto& r = e.renderer_;
        if (!r.live_readback_ || r.rates_.counter_ > 0x77FF || r.noise_ > 0x7FFF ||
            r.echo_history_position_ >= 8 || r.echo_offset_ > 0x7800 || r.echo_offset_ % 4 ||
            r.echo_length_ > 0x7800 || r.echo_length_ % 0x800) return false;
        for (const auto& v : r.voices_) {
            if (v.stream.decoder_.previous_ < -16384 || v.stream.decoder_.previous_ > 16383 ||
                v.stream.decoder_.before_previous_ < -16384 || v.stream.decoder_.before_previous_ > 16383 ||
                v.stream.group_index_ >= 4 || v.ring.phase_ > 0x7FFF ||
                v.ring.write_position_ >= 12 || v.ring.write_position_ % 4 ||
                v.envelope.envelope_ > 0x7FF || v.envelope.preclamp_ < -16384 ||
                v.envelope.preclamp_ > 16384 || static_cast<unsigned>(v.envelope.phase_) > 3 ||
                static_cast<unsigned>(v.sequence.stage_) > 5) return false;
            for (const auto s : v.ring.samples_) if (s < -16384 || s > 16383) return false;
        }
        return true;
    }
public:
    static std::vector<std::uint8_t> save(const SnesDspAudioEngine& e) {
        Writer writer;
        writer.bytes.reserve(70 * 1024);
        writer(signature);
        fields(writer, e);
        return std::move(writer.bytes);
    }
    static bool load(SnesDspAudioEngine& e, const std::vector<std::uint8_t>& bytes) noexcept {
        if (bytes.size() < signature.size() || bytes.size() > 70 * 1024) return false;
        Reader reader{bytes};
        std::array<std::uint8_t, 8> header{};
        reader(header);
        if (header != signature) return false;
        // Validate against a separate bus: neither constructor/reset nor parsing
        // can write registers/RAM or invoke callbacks on the live destination.
        SnesApuBus candidate_bus;
        SnesDspAudioEngine candidate(candidate_bus);
        fields(reader, candidate);
        if (!reader.valid || reader.position != bytes.size() || !valid(candidate)) return false;
        Reader commit{bytes, signature.size()};
        fields(commit, e); // Cannot fail after the identical validated traversal.
        return true;
    }
};

SnesDspAudioEngine::SnesDspAudioEngine(SnesApuBus& bus) noexcept
    : bus_(bus), renderer_(bus), clock_(renderer_, bus) {
    renderer_.set_live_readback_enabled(true);
    reset();
}

void SnesDspAudioEngine::reset() noexcept {
    renderer_.reset();
    clock_.reset();
    buffer_.fill({});
    head_ = size_ = 0;
}

bool SnesDspAudioEngine::clock() noexcept {
    if (size_ == buffer_capacity) return false;
    if (const auto sample = clock_.clock()) {
        buffer_[(head_ + size_) % buffer_capacity] = *sample;
        ++size_;
    }
    return true;
}

std::uint64_t SnesDspAudioEngine::run_clocks(const std::uint64_t limit) noexcept {
    std::uint64_t advanced{};
    while (advanced < limit && clock()) ++advanced;
    return advanced;
}

bool SnesDspAudioEngine::pop_sample(StereoSample& sample) noexcept {
    if (!size_) return false;
    sample = buffer_[head_];
    buffer_[head_] = {};
    head_ = static_cast<std::uint16_t>((head_ + 1) % buffer_capacity);
    --size_;
    return true;
}

std::vector<std::uint8_t> SnesDspAudioEngine::save_state() const {
    return SnesDspStateCodec::save(*this);
}

bool SnesDspAudioEngine::load_state(const std::vector<std::uint8_t>& bytes) noexcept {
    return SnesDspStateCodec::load(*this, bytes);
}

} // namespace gameboy
