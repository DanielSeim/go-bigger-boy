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
struct PortWindow {
    std::uint64_t first{}, last{}, keyon{};
    std::array<unsigned,4> writes{};
};
struct PortHandshake {
    std::uint64_t half{}, first_read{}, last_read{}, first_equal{}, last_equal{};
    unsigned request{}, port{}, equal_reads{}, unequal_reads{};
    std::uint8_t written{}; // Used only for equality; never exported.
    std::uint64_t first_reply{}, last_reply{}, first_equal_reply{}, last_equal_reply{};
    unsigned reply_writes{}, equal_reply_writes{};
    unsigned fields{}, music_reply_writes{};
    std::uint64_t first_music_reply{};
};
struct PitchWrite { std::uint64_t half; unsigned request, pitch; };
struct VolumeWrite { std::uint64_t half; unsigned request, address, value; };
struct DirectoryRead { std::uint64_t half; unsigned request, address; };
struct TimerAccess {
    std::uint64_t first{},last{},min_gap{},max_gap{};
    unsigned count{};
    void add(std::uint64_t half) noexcept {
        if (count) { const auto gap=half-last;
            if (count==1 || gap<min_gap) min_gap=gap;
            if (gap>max_gap) max_gap=gap;
        } else first=half;
        last=half; ++count;
    }
};
struct TimerWindow {
    std::uint64_t directory{},root{};
    std::array<std::array<TimerAccess,3>,2> accesses{};
};
struct Observation {
    Host* host{};
    std::array<Event,128> events{};
    unsigned count{}, kof=256, flg=256;
    bool overflow{};
    std::array<PortWindow,16> ports{};
    bool handshakes_enabled{};
    bool replies_enabled{};
    bool handoff_enabled{};
    bool directory_enabled{};
    bool traversal_enabled{};
    bool volume_enabled{};
    bool timers_enabled{};
    std::array<TimerWindow,16> timers{};
    std::array<VolumeWrite,512> volumes{};
    unsigned volume_count{};
    std::array<DirectoryRead,512> traversal{};
    unsigned traversal_count{};
    std::array<DirectoryRead,128> directory{};
    unsigned directory_count{};
    static void read(void* context,std::uint64_t half,std::uint16_t address) noexcept {
        auto& self=*static_cast<Observation*>(context);
        const bool directory=address>=0x2B00 && address<=0x2B05;
        const bool timer=self.timers_enabled && address>=0xFD && address<=0xFF;
        const bool traversal=(address>=0x2B10 && address<=0x2B1B) ||
            (address>=0x2B20 && address<=0x2B4F) ||
            (address>=0x2B60 && address<=0x2B6C) ||
            (address>=0x2B90 && address<=0x2B9C) ||
            (address>=0x2BC0 && address<=0x2BCC);
        if (!directory && !timer && !(self.traversal_enabled && traversal)) return;
        const auto request=self.host->icd().sound_packets_delivered();
        if (!self.host->icd().audible_sound_packets_delivered() || !request ||
            request>self.ports.size() || self.ports[request-1].keyon) return;
        if (self.timers_enabled) {
            auto& window=self.timers[request-1];
            const auto music=self.host->icd().last_delivered_sound_packet()[4];
            if (music>=1 && music<=3) {
                const auto slot=music-1U;
                if (address==0x2B00+2*slot || address==0x2B01+2*slot) {
                    // Keep only the final directory pass, including owned
                    // firmware that prevalidates the same score first.
                    window={}; window.directory=half;
                } else if (!window.root && window.directory &&
                           address>=0x2B10+4*slot && address<=0x2B13+4*slot) window.root=half;
            }
            if (timer) {
                if (window.directory) {
                    auto& access=window.accesses[window.root?1:0][address-0xFD];
                    if (access.count==1000000) {self.overflow=true;return;}
                    access.add(half);
                }
                return;
            }
        }
        if (!directory) {
            if (self.traversal_count==self.traversal.size()) {self.overflow=true;return;}
            self.traversal[self.traversal_count++]={half,static_cast<unsigned>(request),address};
            return;
        }
        if (self.directory_count==self.directory.size()) {self.overflow=true;return;}
        self.directory[self.directory_count++]={half,static_cast<unsigned>(request),address};
    }
    std::array<PitchWrite,128> pitches{};
    unsigned pitch_count{};
    std::array<PortHandshake,128> handshakes{};
    unsigned handshake_count{};
    std::array<unsigned,4> latest{}; // One-based index of latest write per port.
    static void reply(void* context,std::uint64_t half,std::uint8_t port,std::uint8_t value) noexcept {
        auto& self=*static_cast<Observation*>(context);
        const auto request=self.host->icd().sound_packets_delivered();
        if (!self.replies_enabled || port>3 || !request || request>self.ports.size() ||
            self.ports[request-1].keyon || !self.latest[port]) return;
        auto& entry=self.handshakes[self.latest[port]-1];
        if (entry.request!=request) return;
        if (!entry.first_reply) entry.first_reply=half;
        entry.last_reply=half; ++entry.reply_writes;
        if (self.handoff_enabled && value==self.host->icd().last_delivered_sound_packet()[4]) {
            if (!entry.first_music_reply) entry.first_music_reply=half;
            ++entry.music_reply_writes;
        }
        if (value==entry.written) {
            if (!entry.first_equal_reply) entry.first_equal_reply=half;
            entry.last_equal_reply=half; ++entry.equal_reply_writes;
        }
        if (entry.reply_writes>1000000) self.overflow=true;
    }
    static void port(void* context,std::uint64_t,char kind,std::uint16_t address,std::uint8_t value) noexcept {
        auto& self=*static_cast<Observation*>(context);
        const auto request=self.host->icd().sound_packets_delivered();
        if (!self.host->icd().audible_sound_packets_delivered()) return;
        if (!request || request>self.ports.size() || address<0x2140 || address>0x2143) return;
        auto& window=self.ports[request-1]; if (window.keyon) return;
        // APU clock at the host bus callback, not an exact fractional bus phase.
        const auto half=self.host->apu_half_clocks();
        const auto index=address-0x2140;
        if (self.handshakes_enabled) {
            if (kind=='H') {
                if (self.handshake_count==self.handshakes.size()) {self.overflow=true;return;}
                auto& entry=self.handshakes[self.handshake_count++];
                entry.half=half; entry.request=static_cast<unsigned>(request);
                entry.port=index; entry.written=value;
                if (self.handoff_enabled) {
                    const auto& packet=self.host->icd().last_delivered_sound_packet();
                    for (unsigned field=0;field<4;++field)
                        if (value==packet[std::array<unsigned,4>{4,1,2,3}[field]]) entry.fields|=1U<<field;
                }
                self.latest[index]=self.handshake_count;
            } else if (kind=='h' && self.latest[index]) {
                auto& entry=self.handshakes[self.latest[index]-1];
                // Do not attribute reads in a new request to an older write.
                if (entry.request!=request) return;
                if (!entry.first_read) entry.first_read=half;
                entry.last_read=half;
                if (value==entry.written) {
                    if (!entry.first_equal) entry.first_equal=half;
                    entry.last_equal=half; ++entry.equal_reads;
                } else ++entry.unequal_reads;
                if (entry.equal_reads+entry.unequal_reads>1000000) self.overflow=true;
            }
        }
        if (kind!='H') return;
        if (!window.first) window.first=half;
        window.last=half; ++window.writes[address-0x2140];
    }
    static void write(void* context, std::uint64_t half, std::uint8_t address, std::uint8_t value) noexcept {
        auto& self=*static_cast<Observation*>(context);
        const auto requests=self.host->icd().sound_packets_delivered();
        if (!self.host->icd().audible_sound_packets_delivered()) return;
        if (self.volume_enabled && requests && requests<=self.ports.size() &&
            !self.ports[requests-1].keyon &&
            (address==0x0c || address==0x1c || address==0x20 || address==0x21)) {
            if (self.volume_count==self.volumes.size()) {self.overflow=true;return;}
            self.volumes[self.volume_count++]={half,static_cast<unsigned>(requests),address,value};
        }
        if (self.handoff_enabled && (address==0x22 || address==0x23) && requests &&
            requests<=self.ports.size() && !self.ports[requests-1].keyon) {
            if (self.pitch_count==self.pitches.size()) {self.overflow=true;return;}
            const auto pitch=unsigned(self.host->debug_dsp_register(0x22)) |
                (unsigned(self.host->debug_dsp_register(0x23))<<8);
            self.pitches[self.pitch_count++]={half,static_cast<unsigned>(requests),pitch};
        }
        if (address==0x5c) { if (self.kof==value) return; self.kof=value; }
        else if (address==0x6c) { if (self.flg==value) return; self.flg=value; }
        else if (address!=0x4c || !value) return;
        if (address==0x4c && requests && requests<=self.ports.size()) {
            auto& window=self.ports[requests-1];
            if (!window.keyon) window.keyon=half;
        }
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
    if (argc!=7 && (argc!=8 || (std::string(argv[7])!="--port-handshakes" && std::string(argv[7])!="--spc-replies" && std::string(argv[7])!="--selection-handoff" && std::string(argv[7])!="--selection-directory" && std::string(argv[7])!="--selection-traversal" && std::string(argv[7])!="--selection-volume" && std::string(argv[7])!="--selection-timers")))
        throw std::runtime_error("PROGRAM GAME sgb|sgb2 CLOCKS SCRIPT bundled|fixture [--port-handshakes|--spc-replies|--selection-handoff|--selection-directory|--selection-traversal|--selection-volume|--selection-timers]");
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
    observed.handshakes_enabled=argc==8;
    observed.volume_enabled=argc==8 && std::string(argv[7])=="--selection-volume";
    observed.timers_enabled=argc==8 && std::string(argv[7])=="--selection-timers";
    observed.traversal_enabled=observed.timers_enabled || observed.volume_enabled || (argc==8 && std::string(argv[7])=="--selection-traversal");
    observed.directory_enabled=observed.traversal_enabled || (argc==8 && std::string(argv[7])=="--selection-directory");
    if (observed.directory_enabled) host.debug_set_spc_read_observer(Observation::read,&observed);
    observed.handoff_enabled=observed.directory_enabled || (argc==8 && std::string(argv[7])=="--selection-handoff");
    observed.replies_enabled=observed.handoff_enabled || (argc==8 && std::string(argv[7])=="--spc-replies");
    if (observed.replies_enabled) host.debug_set_spc_port_write_observer(Observation::reply,&observed);
    host.debug_set_dsp_write_observer(Observation::write,&observed);
    host.debug_set_apu_port_observer(Observation::port,&observed);
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
    std::cout<<"],\"port_windows\":[";
    bool first=true;
    for (unsigned i=0;i<count;++i) {
        const auto& w=observed.ports[i]; if (!w.keyon) continue;
        std::cout<<(first?"":",")<<"{\"request\":"<<i+1<<",\"first\":"<<w.first
                 <<",\"last\":"<<w.last<<",\"keyon\":"<<w.keyon<<",\"writes\":[";
        first=false;
        for (unsigned j=0;j<4;++j) std::cout<<(j?",":"")<<w.writes[j];
        std::cout<<"]}";
    }
    std::cout<<"]";
    if (observed.handshakes_enabled) {
        std::cout<<",\"port_handshakes\":[";
        for (unsigned i=0;i<observed.handshake_count;++i) {
            const auto& h=observed.handshakes[i];
            std::cout<<(i?",":"")<<"{\"request\":"<<h.request<<",\"port\":"<<h.port
                <<",\"half\":"<<h.half<<",\"first_read\":"<<h.first_read<<",\"last_read\":"<<h.last_read
                <<",\"first_equal\":"<<h.first_equal<<",\"last_equal\":"<<h.last_equal
                <<",\"equal_reads\":"<<h.equal_reads<<",\"unequal_reads\":"<<h.unequal_reads<<"}";
        }
        std::cout<<"]";
    }
    if (observed.replies_enabled) {
        std::cout<<",\"spc_reply_handshakes\":[";
        for (unsigned i=0;i<observed.handshake_count;++i) {
            const auto& h=observed.handshakes[i];
            std::cout<<(i?",":"")<<"{\"request\":"<<h.request<<",\"port\":"<<h.port<<",\"half\":"<<h.half
                <<",\"first_reply\":"<<h.first_reply<<",\"last_reply\":"<<h.last_reply
                <<",\"first_equal_reply\":"<<h.first_equal_reply<<",\"last_equal_reply\":"<<h.last_equal_reply
                <<",\"reply_writes\":"<<h.reply_writes<<",\"equal_reply_writes\":"<<h.equal_reply_writes<<"}";
        }
        std::cout<<"]";
    }
    if (observed.handoff_enabled) {
        std::cout<<",\"handoff_writes\":[";
        for (unsigned i=0;i<observed.handshake_count;++i) {
            const auto& h=observed.handshakes[i];
            std::cout<<(i?",":"")<<"{\"request\":"<<h.request<<",\"port\":"<<h.port<<",\"half\":"<<h.half
                <<",\"fields\":"<<h.fields<<",\"first_music_reply\":"<<h.first_music_reply
                <<",\"music_reply_writes\":"<<h.music_reply_writes<<"}";
        }
        std::cout<<"],\"selection_pitch_writes\":[";
        for (unsigned i=0;i<observed.pitch_count;++i) {
            const auto& p=observed.pitches[i];
            std::cout<<(i?",":"")<<"{\"request\":"<<p.request<<",\"half\":"<<p.half<<",\"pitch\":"<<p.pitch<<"}";
        }
        std::cout<<"]";
    }
    if (observed.directory_enabled) {
        std::cout<<",\"selection_directory_reads\":[";
        for (unsigned i=0;i<observed.directory_count;++i) {
            const auto& r=observed.directory[i];
            std::cout<<(i?",":"")<<"{\"request\":"<<r.request<<",\"half\":"<<r.half
                <<",\"address\":"<<r.address<<"}";
        }
        std::cout<<"]";
    }
    if (observed.traversal_enabled) {
        std::cout<<",\"selection_traversal_reads\":[";
        for (unsigned i=0;i<observed.traversal_count;++i) {
            const auto& r=observed.traversal[i];
            std::cout<<(i?",":"")<<"{\"request\":"<<r.request<<",\"half\":"<<r.half
                <<",\"address\":"<<r.address<<"}";
        }
        std::cout<<"]";
    }
    if (observed.volume_enabled) {
        std::cout<<",\"selection_volume_writes\":[";
        for (unsigned i=0;i<observed.volume_count;++i) {
            const auto& v=observed.volumes[i];
            std::cout<<(i?",":"")<<"{\"request\":"<<v.request<<",\"half\":"<<v.half
                <<",\"register\":"<<v.address<<",\"value\":"<<v.value<<"}";
        }
        std::cout<<"]";
    }
    if (observed.timers_enabled) {
        std::cout<<",\"selection_timer_reads\":[";
        bool first_window=true;
        for (unsigned i=0;i<count;++i) {
            const auto& w=observed.timers[i]; if (!w.directory || !w.root) continue;
            std::cout<<(first_window?"":",")<<"{\"request\":"<<i+1
                <<",\"directory_half\":"<<w.directory<<",\"root_half\":"<<w.root;
            first_window=false;
            for (unsigned stage=0;stage<2;++stage) {
                std::cout<<(stage?",\"setup\":[":",\"wait\":[");
                for (unsigned timer=0;timer<3;++timer) {
                    const auto& a=w.accesses[stage][timer];
                    std::cout<<(timer?",":"")<<"{\"address\":"<<0xFD+timer<<",\"count\":"<<a.count
                        <<",\"first\":"<<a.first<<",\"last\":"<<a.last
                        <<",\"min_gap\":"<<a.min_gap<<",\"max_gap\":"<<a.max_gap<<"}";
                }
                std::cout<<"]";
            }
            std::cout<<"}";
        }
        std::cout<<"]";
    }
    std::cout<<",\"final\":{\"flg\":"<<unsigned(host.debug_dsp_register(0x6c))
             <<",\"kof\":"<<unsigned(host.debug_dsp_register(0x5c))
             <<",\"echo_left\":"<<unsigned(host.debug_dsp_register(0x2c))
             <<",\"echo_right\":"<<unsigned(host.debug_dsp_register(0x3c))<<"}}\n";
 } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 2; }
}
