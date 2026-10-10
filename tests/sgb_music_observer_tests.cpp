// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/sgb_host.hpp"
#include "../firmware/sgb/prototype_image.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using Host=gameboy::SgbHost;
void require(bool value) {if(!value) throw std::runtime_error("DSP observer lifecycle mismatch");}
std::vector<Host::StereoSample> run(Host& host) {
    std::vector<Host::StereoSample> output;
    while(host.cpu().timing().clocks()<12000000) {
        require(host.step()); Host::StereoSample sample;
        while(host.pop_sample(sample)) output.push_back(sample);
    }
    return output;
}
int main() {
 try {
    gameboy::SgbHostConfig config;
    config.program_rom=gameboy::firmware::sgb_prototype_rom();
    config.game_rom.resize(32768); config.game_rom[0x100]=0x18; config.game_rom[0x101]=0xfe;
    config.gb_boot_rom[0]=0xc3; config.gb_boot_rom[2]=1;
    Host observed(config), plain(config), restored(config);
    unsigned writes{}, destination_writes{};
    const auto callback=+[](void* context,std::uint64_t,std::uint8_t,std::uint8_t) noexcept {++*static_cast<unsigned*>(context);};
    observed.debug_set_dsp_write_observer(callback,&writes);
    restored.debug_set_dsp_write_observer(callback,&destination_writes);
    const auto initial=observed.save_state(); require(initial==plain.save_state());
    const auto audio=run(observed), expected=run(plain);
    const auto same=[](const auto& a,const auto& b) {return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](auto x,auto y){return x.left==y.left && x.right==y.right;});};
    require(writes>0 && same(audio,expected) && observed.save_state()==plain.save_state());
    const auto first=writes;
    require(observed.load_state(initial)); require(same(audio,run(observed)) && writes==2*first);
    require(restored.load_state(initial)); require(same(audio,run(restored)) && destination_writes==first && writes==2*first);
    observed.reset(); require(same(audio,run(observed)) && writes==3*first);
    observed.debug_set_dsp_write_observer(nullptr,nullptr); observed.reset();
    require(same(audio,run(observed)) && writes==3*first);
    std::cout<<"DSP observation preserves host PCM/state and destination reset/load bindings\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
