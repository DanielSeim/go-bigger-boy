// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded register-only observation of caller-owned programs and games.
#include "gameboy/sgb_host.hpp"
#include "gameboy/boot_rom.hpp"
#include "../scripts/sgb_input_script.h"
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Host = gameboy::SgbHost;
struct Event { std::uint64_t half; unsigned kind, value, request, pitch, srcn, adsr1, adsr2, gain; };
struct Observation {
    Host* host{};
    std::array<Event,128> events{};
    unsigned count{}, kof=256, flg=256;
    bool overflow{};
    static void write(void* context, std::uint64_t half, std::uint8_t address, std::uint8_t value) noexcept {
        auto& self=*static_cast<Observation*>(context);
        const auto requests=self.host->icd().sound_packets_delivered();
        if (!self.host->icd().audible_sound_packets_delivered()) return;
        if (address==0x5c) { if (self.kof==value) return; self.kof=value; }
        else if (address==0x6c) { if (self.flg==value) return; self.flg=value; }
        else if (address!=0x4c || !value) return;
        if (self.count==self.events.size()) { self.overflow=true; return; }
        const auto reg=[&](unsigned a) { return unsigned(self.host->debug_dsp_register(a)); };
        self.events[self.count++]={half,address,value,static_cast<unsigned>(requests),reg(0x22)|(reg(0x23)<<8),reg(0x24),reg(0x25),reg(0x26),reg(0x27)};
    }
};
std::vector<std::uint8_t> read(const char* path, std::size_t low, std::size_t high) {
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    if (!stream || stream.tellg()<std::streamoff(low) || stream.tellg()>std::streamoff(high)) throw std::runtime_error("invalid image");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(stream.tellg())); stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(data.data()),data.size())) throw std::runtime_error("image read failed");
    return data;
}
int main(int argc,char** argv) {
 try {
    if (argc!=7) throw std::runtime_error("PROGRAM GAME sgb|sgb2 CLOCKS SCRIPT bundled|fixture");
    gameboy::SgbHostConfig config;
    config.program_rom=read(argv[1],262144,524288); config.game_rom=read(argv[2],336,16*1024*1024);
    const std::string model=argv[3], boot=argv[6];
    if (model!="sgb" && model!="sgb2") throw std::runtime_error("invalid model");
    config.model=model=="sgb"?gameboy::HardwareModel::sgb:gameboy::HardwareModel::sgb2;
    if (boot=="bundled") config.gb_boot_rom=model=="sgb"?gameboy::sgb_boot_rom():gameboy::sgb2_boot_rom();
    else if (boot=="fixture") {config.gb_boot_rom[0]=0xc3;config.gb_boot_rom[2]=1;}
    else throw std::runtime_error("invalid bootstrap");
    std::uint64_t target{}; const std::string number=argv[4];
    auto parsed=std::from_chars(number.data(),number.data()+number.size(),target);
    if (parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || !target || target>2000000000) throw std::runtime_error("invalid clock bound");
    gbb_sgb_input_script inputs{}; char error[256]{};
    if (!gbb_sgb_input_load(argv[5],&inputs,error,sizeof(error))) throw std::runtime_error("invalid inputs");
    for (std::size_t i=0;i<inputs.count;++i) config.input_events.push_back({inputs.events[i].frame,inputs.events[i].mask});
    Host host(config); Observation observed; observed.host=&host;
    host.debug_set_dsp_write_observer(Observation::write,&observed);
    std::array<std::uint64_t,16> requests{}; std::array<unsigned,16> music{}; unsigned count{};
    while (host.cpu().timing().clocks()<target) {
        if (!host.step()) throw std::runtime_error("host execution fault");
        Host::StereoSample sample; while (host.pop_sample(sample)) {}
        const auto delivered=host.icd().sound_packets_delivered();
        if (delivered>count) {
            if (delivered!=count+1 || count==requests.size()) throw std::runtime_error("request bound exceeded");
            music[count]=host.icd().last_delivered_sound_packet()[4];
            requests[count++]=host.apu_half_clocks();
        }
    }
    if (observed.overflow) throw std::runtime_error("event bound exceeded");
    std::cout<<"{\"schema\":\"gbb-sgb-music-timeline-v1\",\"qualification\":false,\"model\":\""<<model<<"\",\"requests\":[";
    for (unsigned i=0;i<count;++i) std::cout<<(i?",":"")<<requests[i];
    std::cout<<"],\"music\":[";
    for (unsigned i=0;i<count;++i) std::cout<<(i?",":"")<<music[i];
    std::cout<<"],\"events\":[";
    for (unsigned i=0;i<observed.count;++i) {const auto& e=observed.events[i];
        std::cout<<(i?",":"")<<"{\"half\":"<<e.half<<",\"register\":"<<e.kind<<",\"value\":"<<e.value<<",\"request\":"<<e.request;
        if (e.kind==0x4c) std::cout<<",\"pitch\":"<<e.pitch<<",\"srcn\":"<<e.srcn<<",\"adsr1\":"<<e.adsr1<<",\"adsr2\":"<<e.adsr2<<",\"gain\":"<<e.gain;
        std::cout<<"}";
    }
    std::cout<<"],\"final\":{\"flg\":"<<unsigned(host.debug_dsp_register(0x6c))
             <<",\"kof\":"<<unsigned(host.debug_dsp_register(0x5c))
             <<",\"echo_left\":"<<unsigned(host.debug_dsp_register(0x2c))
             <<",\"echo_right\":"<<unsigned(host.debug_dsp_register(0x3c))<<"}}\n";
 } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 2; }
}
