// SPDX-License-Identifier: GPL-3.0-or-later
// Owned SPC mailbox test: request reload while the timer wait is active.
#include "gameboy/snes_audio_host.hpp"
#include "gameboy/snes_spc700.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
void checked(bool ok,int line) { if (!ok) throw std::runtime_error("owned mid-wait reload contract failed at line "+std::to_string(line)); }
#define require(ok) checked((ok),__LINE__)
std::vector<unsigned char> read(const char* name,unsigned size) {
    std::ifstream in(name,std::ios::binary|std::ios::ate);
    require(bool(in) && in.tellg()==std::streamoff(size));
    std::vector<unsigned char> data(size);in.seekg(0);require(bool(in.read(reinterpret_cast<char*>(data.data()),size)));return data;
}
}
int main(int argc,char** argv) {
 try {
    require(argc==3);const auto rom=read(argv[1],262144),game=read(argv[2],32768);
    gameboy::SnesApuBus bus;
    // Independent entry stub. Observe the subsequent return to IPL before
    // executing it; this is a mailbox ownership test, not an IPL upload test.
    gameboy::SnesApuBus::IplRom entry{};entry[0]=0x5f;entry[1]=0;entry[2]=4;
    bus.install_ipl(entry);
    for (unsigned i=0;i<0x1900;++i) bus.dsp_write_ram(0x200+i,rom[0x1000+i]);
    for (unsigned i=0;i<256;++i) bus.dsp_write_ram(0x2b00+i,game[0x4004+i]);
    bus.dsp_write_ram(0x508,127);bus.dsp_write_ram(0x510,0);bus.dsp_write_ram(0x511,0x2c);
    gameboy::SnesSpc700 cpu(bus);cpu.set_cycle_bus_enabled(true);
    const auto until=[&](auto condition) {
        for (unsigned i=0;i<100000;++i) {if(condition()) return;require(cpu.step().supported);}
        require(false);
    };
    until([&]{return bus.host_read_port(0)==0x5a;});
    until([&]{return bus.host_read_port(0)==0 && bus.host_read_port(1)==0xdb;});
    const auto select=[&](unsigned music,unsigned token) {
        bus.host_write_port(1,0);bus.host_write_port(2,music);bus.host_write_port(3,2);bus.host_write_port(0,token);
        until([&]{return bus.host_read_port(0)==token;});
        bus.host_write_port(1,0);bus.host_write_port(2,0);bus.host_write_port(3,0);bus.host_write_port(0,token+1);
    };
    select(1,1);until([&]{return bus.dsp_read_ram(0xd7)==1;});
    require(bus.dsp_read_ram(0x7b)==32);
    select(2,3);until([&]{return bus.dsp_read_ram(0x7c)==1;});
    const auto start=cpu.cycles();
    bus.host_write_port(3,1);bus.host_write_port(0,5);
    until([&]{return cpu.registers().pc==0xffc0;});
    require(cpu.cycles()-start<1024 && bus.dsp_read_ram(0x7b)==0 && bus.dsp_read_ram(0x7c)==0);
    require(bus.dsp_read_ram(0xd7)==1 && bus.dsp_read_ram(0xd2)==0 && bus.dsp_read_ram(0xf1)==0x80);
    std::cout<<"{\"mid_wait_reload\":true,\"stale_keyon\":false,\"cycles\":"<<cpu.cycles()-start<<"}\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 2;}
}
