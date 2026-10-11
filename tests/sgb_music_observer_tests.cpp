// SPDX-License-Identifier: GPL-3.0-or-later
#include "gameboy/sgb_host.hpp"
#include "../firmware/sgb/prototype_image.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using Host=gameboy::SgbHost;
struct Replies {
    unsigned count{};
    std::uint64_t hash=14695981039346656037ULL;
    static void write(void* context,std::uint64_t half,std::uint8_t port,std::uint8_t value) noexcept {
        auto& self=*static_cast<Replies*>(context); ++self.count;
        for (const auto word : {half,std::uint64_t(port),std::uint64_t(value)})
            self.hash=(self.hash^word)*1099511628211ULL;
    }
};
void require(bool value) {if(!value) throw std::runtime_error("DSP observer lifecycle mismatch");}
std::vector<Host::StereoSample> run(Host& host,std::uint64_t target=12000000) {
    std::vector<Host::StereoSample> output;
    while(host.cpu().timing().clocks()<target) {
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
    unsigned ports{}, destination_ports{};
    const auto port_callback=+[](void* context,std::uint64_t,char,std::uint16_t,std::uint8_t) noexcept {++*static_cast<unsigned*>(context);};
    observed.debug_set_apu_port_observer(port_callback,&ports);
    restored.debug_set_apu_port_observer(port_callback,&destination_ports);
    Replies replies, destination_replies;
    observed.debug_set_spc_port_write_observer(Replies::write,&replies);
    restored.debug_set_spc_port_write_observer(Replies::write,&destination_replies);
    const auto initial=observed.save_state(); require(initial==plain.save_state());
    const auto audio=run(observed), expected=run(plain);
    const auto same=[](const auto& a,const auto& b) {return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](auto x,auto y){return x.left==y.left && x.right==y.right;});};
    require(writes>0 && same(audio,expected) && observed.save_state()==plain.save_state());
    const auto first=writes;
    const auto first_ports=ports; require(first_ports>0);
    const auto first_replies=replies; require(first_replies.count>0); replies={};
    Host scalar(config); Replies scalar_replies;
    scalar.debug_set_apu_batch_enabled(false);
    scalar.debug_set_spc_port_write_observer(Replies::write,&scalar_replies);
    require(same(audio,run(scalar)) && scalar.save_state()==plain.save_state());
    require(scalar_replies.count==first_replies.count && scalar_replies.hash==first_replies.hash);
    Host mid(config), continued(config); Replies mid_replies, continued_replies;
    mid.debug_set_spc_port_write_observer(Replies::write,&mid_replies);
    (void)run(mid,100000);
    const auto during_upload=mid.save_state(); mid_replies={};
    const auto continuation=run(mid);
    continued.debug_set_spc_port_write_observer(Replies::write,&continued_replies);
    require(continued.load_state(during_upload));
    require(same(continuation,run(continued)) && mid.save_state()==continued.save_state());
    require(mid_replies.count>0 && mid_replies.count==continued_replies.count &&
            mid_replies.hash==continued_replies.hash);
    require(observed.load_state(initial)); require(same(audio,run(observed)) && writes==2*first);
    require(ports==2*first_ports);
    require(replies.count==first_replies.count && replies.hash==first_replies.hash);
    require(restored.load_state(initial)); require(same(audio,run(restored)) && destination_writes==first && writes==2*first);
    require(destination_ports==first_ports && ports==2*first_ports);
    require(destination_replies.count==first_replies.count && destination_replies.hash==first_replies.hash);
    replies={};
    observed.reset(); require(same(audio,run(observed)) && writes==3*first);
    require(ports==3*first_ports);
    require(replies.count==first_replies.count && replies.hash==first_replies.hash);
    observed.debug_set_dsp_write_observer(nullptr,nullptr); observed.reset();
    observed.debug_set_apu_port_observer(nullptr,nullptr);
    observed.debug_set_spc_port_write_observer(nullptr,nullptr);
    require(same(audio,run(observed)) && writes==3*first);
    require(ports==3*first_ports);
    require(replies.count==first_replies.count && replies.hash==first_replies.hash);
    std::cout<<"DSP/port observation preserves host PCM/state and destination reset/load bindings\n";
 } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
