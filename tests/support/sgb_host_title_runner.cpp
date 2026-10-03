#include "gameboy/sgb_host.hpp"
#include "sgb_input_script.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <algorithm>
#include <charconv>
#include <optional>
#include <cstring>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {
// The nested GB state includes CPU/peripherals, raw capture and displayed
// framebuffers. Hash only that component: SPC replay bookkeeping is allowed
// to differ, but GB state/pixels must remain identical across optimizations.
std::uint64_t gb_state_hash(const gameboy::SgbHost& host) {
    const auto state = host.save_state();
    const auto end = state.size() - 8;
    for (std::size_t i = 8; i + 28 <= end; ++i) {
        if (std::memcmp(state.data()+i,"GBBSTATE",8)) continue;
        std::uint64_t length{}; unsigned payload{};
        for (unsigned j=0;j<8;++j) length |= std::uint64_t(state[i-8+j]) << (8*j);
        for (unsigned j=0;j<4;++j) payload |= unsigned(state[i+20+j]) << (8*j);
        if (length != end-i || length != std::uint64_t(payload)+28) continue;
        std::uint64_t hash=14695981039346656037ULL;
        for (std::size_t j=i;j<end;++j) { hash ^= state[j]; hash *= 1099511628211ULL; }
        return hash;
    }
    throw std::runtime_error("nested GB state was not found");
}
std::optional<double> process_cpu_seconds() noexcept {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF,&usage)==0)
        return usage.ru_utime.tv_sec+usage.ru_stime.tv_sec+
            (usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)/1000000.0;
#endif
    return std::nullopt;
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("missing local image");
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    if(input.bad()) throw std::runtime_error("could not read complete local image");
    return bytes;
}
template<std::size_t N> std::array<std::uint8_t,N> image(const std::filesystem::path& path) {
    auto bytes=read_file(path); if(bytes.size()!=N) throw std::runtime_error("wrong image size");
    std::array<std::uint8_t,N> result{}; std::copy(bytes.begin(),bytes.end(),result.begin()); return result;
}
inline void write_pcm_wav(const std::filesystem::path& path,
                   const std::vector<gameboy::SgbHost::StereoSample>& samples,
                   unsigned sample_rate) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("PCM output already exists: " + path.string());
    if (samples.size() > (UINT32_MAX - 44U) / 4U)
        throw std::runtime_error("PCM output exceeds WAV size limit");
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("could not create PCM output: " + path.string());
    const auto u16 = [&output](std::uint16_t value) {
        output.put(static_cast<char>(value));
        output.put(static_cast<char>(value >> 8));
    };
    const auto u32 = [&output](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            output.put(static_cast<char>(value >> shift));
    };
    const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 4U);
    output.write("RIFF", 4);
    u32(data_bytes + 36U);
    output.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(sample_rate);
    u32(sample_rate * 4U);
    u16(4);
    u16(16);
    output.write("data", 4);
    u32(data_bytes);
    for (const auto& sample : samples) {
        u16(static_cast<std::uint16_t>(sample.left));
        u16(static_cast<std::uint16_t>(sample.right));
    }
    if (!output) throw std::runtime_error("could not finish PCM output: " + path.string());
}

} // namespace
int main(int argc,char** argv) {
    if(argc<9) return 2;
    try {
        bool restore{}, combined{}, scalar_apu{}, scalar_spc{};
        unsigned rate=48000, chunk=1;
        std::uint64_t steps=60000000;
        const auto number=[](std::string_view text) {
            std::uint64_t value{};
            const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
            if(result.ec!=std::errc{} || result.ptr!=text.data()+text.size())
                throw std::runtime_error("invalid numeric option");
            return value;
        };
        for(int n=9;n<argc;++n) {
            const std::string_view option(argv[n]);
            if(option=="--restore") restore=true;
            else if(option=="--combined") combined=true;
            else if(option=="--scalar-apu") scalar_apu=true;
            else if(option=="--scalar-spc") scalar_spc=true;
            else if((option=="--output-hz" || option=="--chunk" || option=="--steps") && n+1<argc) {
                const auto value=number(argv[++n]);
                if(option=="--steps") { if(!value || value>60000000) throw std::runtime_error("invalid step budget"); steps=value; }
                else if(option=="--chunk") { if(!value || value>1024) throw std::runtime_error("invalid consumer chunk"); chunk=static_cast<unsigned>(value); }
                else { if(value<8000 || value>48000) throw std::runtime_error("invalid sample rate"); rate=static_cast<unsigned>(value); }
            } else throw std::runtime_error("unknown runner option");
        }
        const std::string_view model(argv[1]);
        if(model!="sgb1" && model!="sgb2") return 2;
        if(std::filesystem::exists(argv[7]) || std::filesystem::exists(argv[8]))
            throw std::runtime_error("output paths must be unused");
        if(std::filesystem::absolute(argv[7]).lexically_normal()==std::filesystem::absolute(argv[8]).lexically_normal())
            throw std::runtime_error("WAV and report paths must be distinct");
        gameboy::SgbHostConfig config;
        config.combined_audio=combined; config.output_hz=rate;
        config.model=model=="sgb1"?gameboy::HardwareModel::sgb:gameboy::HardwareModel::sgb2;
        config.program_rom=read_file(argv[2]); config.spc_ipl=image<64>(argv[3]);
        config.game_rom=read_file(argv[4]); config.gb_boot_rom=image<256>(argv[5]);
        auto save_path=std::filesystem::path(argv[4]); save_path.replace_extension(".sav");
        if(std::filesystem::exists(save_path)) config.battery_ram=read_file(save_path);
        gbb_sgb_input_script script{}; char error[128]{};
        if(!gbb_sgb_input_load(argv[6],&script,error,sizeof(error))) throw std::runtime_error(error);
        for(std::size_t n=0;n<script.count;++n) config.input_events.push_back({script.events[n].frame,script.events[n].mask});
        gameboy::SgbHost host(std::move(config));
        host.debug_set_apu_batch_enabled(!scalar_apu);
        host.debug_set_spc_idle_tail_cache_enabled(!scalar_spc);
        std::vector<gameboy::SgbHost::StereoSample> pcm;
        pcm.reserve(combined?6000000:4000000);
        std::uint64_t restorations{},nonzero{};
        const auto cpu_start=process_cpu_seconds();
        const auto start=std::chrono::steady_clock::now();
        struct Window { std::uint64_t clocks; double seconds; };
        std::vector<Window> windows; windows.reserve(512);
        bool windows_complete=true;
        std::uint64_t window_clock{}, next_window=21477273;
        auto window_start=start;
        const auto consume=[&](bool flush) {
            gameboy::SgbHost::StereoSample sample;
            while(host.pending_samples() >= (flush?1:chunk)) {
                const auto count=flush?host.pending_samples():chunk;
                for(std::size_t n=0;n<count;++n) {
                if(!host.pop_sample(sample)) throw std::runtime_error("consumer lost pending sample");
                pcm.push_back(sample); if(sample.left || sample.right) ++nonzero;
                if(restore && pcm.size()%8192==0) {
                    const auto state=host.save_state();
                    if(!host.load_state(state)) throw std::runtime_error("whole host restore rejected");
                    ++restorations;
                }
                }
            }
        };
        for(std::uint64_t n=0;n<steps;++n) {
            if(!host.step()) throw std::runtime_error("host stopped at step "+std::to_string(n)+
                " status "+std::to_string(static_cast<unsigned>(host.status()))+
                " address "+std::to_string(host.fault().address));
            consume(false);
            const auto clock=host.cpu().timing().clocks();
            if(clock>=next_window) {
                const auto now=std::chrono::steady_clock::now();
                if(windows.size()<512) windows.push_back({clock-window_clock,
                    std::chrono::duration<double>(now-window_start).count()});
                else windows_complete=false;
                window_start=now; window_clock=clock; next_window=clock+21477273;
            }
        }
        consume(true);
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        const auto cpu_end=process_cpu_seconds();
        const auto gb_hash=gb_state_hash(host); // Outside timed playback.
        write_pcm_wav(argv[7],pcm,host.sample_rate());
        std::ofstream report(argv[8]);
        report<<"{\"format\":\"gbb-sgb-host-performance-v1\",\"model\":\""<<model<<"\",\"steps\":"<<host.cpu().steps()
              <<",\"master_clocks\":"<<host.cpu().timing().clocks()<<",\"apu_half_clocks\":"<<host.apu_half_clocks()
              <<",\"samples\":"<<pcm.size()<<",\"nonzero\":"<<nonzero<<",\"gb_frames\":"<<host.icd().completed_frames()
              <<",\"inputs\":"<<host.icd().input_events_applied()<<",\"sound_delivered\":"<<host.icd().sound_packets_delivered()
              <<",\"audible_delivered\":"<<host.icd().audible_sound_packets_delivered()<<",\"restorations\":"<<restorations
              <<",\"combined\":"<<(combined?"true":"false")<<",\"output_hz\":"<<host.sample_rate()
              <<",\"gb_samples\":"<<host.gb_samples_captured()<<",\"snes_samples\":"<<host.snes_samples_produced()
              <<",\"clipped_samples\":"<<host.clipped_samples()<<",\"consumer_chunk\":"<<chunk
              <<",\"seconds\":"<<seconds<<",\"realtime_ratio\":"<<(host.cpu().timing().clocks()/21477273.0/seconds)
              <<",\"cpu_seconds\":";
        if(cpu_start && cpu_end && *cpu_end>*cpu_start) {
            const auto cpu_seconds=*cpu_end-*cpu_start;
            report<<cpu_seconds<<",\"cpu_realtime_ratio\":"<<(host.cpu().timing().clocks()/21477273.0/cpu_seconds);
        } else report<<"null,\"cpu_realtime_ratio\":null";
        report<<",\"apu_batched\":"<<(scalar_apu?"false":"true")
              <<",\"spc_idle_tail_cached\":"<<(scalar_spc?"false":"true")
              <<",\"gb_state_hash\":"<<gb_hash
              <<",\"windows_complete\":"<<(windows_complete?"true":"false")<<",\"windows\":[";
        for(std::size_t n=0;n<windows.size();++n) {
            if(n) report<<',';
            report<<"{\"master_clocks\":"<<windows[n].clocks<<",\"seconds\":"<<windows[n].seconds<<'}';
        }
        report<<"]}\n";
        if(!report) throw std::runtime_error("failed to write host report");
        std::cout<<model<<": whole host "<<(restore?"restored":"uninterrupted")<<" completed, "<<seconds<<" seconds\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
