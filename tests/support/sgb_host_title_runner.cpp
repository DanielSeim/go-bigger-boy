#include "gameboy/sgb_host.hpp"
#include "sgb_input_script.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>

namespace {
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("missing local image");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
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
    if(argc!=9 && argc!=10) return 2;
    const bool restore=argc==10 && std::string_view(argv[9])=="--restore";
    if(argc==10 && !restore) return 2;
    try {
        const std::string_view model(argv[1]);
        if(model!="sgb1" && model!="sgb2") return 2;
        if(std::filesystem::exists(argv[7]) || std::filesystem::exists(argv[8]))
            throw std::runtime_error("output paths must be unused");
        gameboy::SgbHostConfig config;
        config.model=model=="sgb1"?gameboy::HardwareModel::sgb:gameboy::HardwareModel::sgb2;
        config.program_rom=read_file(argv[2]); config.spc_ipl=image<64>(argv[3]);
        config.game_rom=read_file(argv[4]); config.gb_boot_rom=image<256>(argv[5]);
        auto save_path=std::filesystem::path(argv[4]); save_path.replace_extension(".sav");
        if(std::filesystem::exists(save_path)) config.battery_ram=read_file(save_path);
        gbb_sgb_input_script script{}; char error[128]{};
        if(!gbb_sgb_input_load(argv[6],&script,error,sizeof(error))) throw std::runtime_error(error);
        for(std::size_t n=0;n<script.count;++n) config.input_events.push_back({script.events[n].frame,script.events[n].mask});
        gameboy::SgbHost host(std::move(config));
        std::vector<gameboy::SgbHost::StereoSample> pcm;
        pcm.reserve(4000000);
        std::uint64_t restorations{},nonzero{};
        const auto start=std::chrono::steady_clock::now();
        for(std::uint64_t n=0;n<60000000;++n) {
            if(!host.step()) throw std::runtime_error("host stopped at step "+std::to_string(n)+
                " status "+std::to_string(static_cast<unsigned>(host.status())));
            gameboy::SgbHost::StereoSample sample;
            while(host.pop_sample(sample)) {
                pcm.push_back(sample); if(sample.left || sample.right) ++nonzero;
                if(restore && pcm.size()%8192==0) {
                    const auto state=host.save_state();
                    if(!host.load_state(state)) throw std::runtime_error("whole host restore rejected");
                    ++restorations;
                }
            }
        }
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        write_pcm_wav(argv[7],pcm,32000);
        std::ofstream report(argv[8]);
        report<<"{\"format\":\"gbb-sgb-host-performance-v1\",\"model\":\""<<model<<"\",\"steps\":"<<host.cpu().steps()
              <<",\"master_clocks\":"<<host.cpu().timing().clocks()<<",\"apu_half_clocks\":"<<host.apu_half_clocks()
              <<",\"samples\":"<<pcm.size()<<",\"nonzero\":"<<nonzero<<",\"gb_frames\":"<<host.icd().completed_frames()
              <<",\"inputs\":"<<host.icd().input_events_applied()<<",\"sound_delivered\":"<<host.icd().sound_packets_delivered()
              <<",\"audible_delivered\":"<<host.icd().audible_sound_packets_delivered()<<",\"restorations\":"<<restorations
              <<",\"seconds\":"<<seconds<<",\"realtime_ratio\":"<<(host.cpu().timing().clocks()/21477273.0/seconds)<<"}\n";
        if(!report) throw std::runtime_error("failed to write host report");
        std::cout<<model<<": whole host "<<(restore?"restored":"uninterrupted")<<" completed, "<<seconds<<" seconds\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
