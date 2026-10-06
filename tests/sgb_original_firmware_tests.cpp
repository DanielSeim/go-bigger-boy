#include "gameboy/sgb_host.hpp"
#include "gameboy/boot_rom.hpp"
#include "../firmware/sgb/prototype_image.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using Host = gameboy::SgbHost;
using Packet = std::array<std::uint8_t, 16>;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void append(std::vector<std::uint8_t>& code, std::initializer_list<std::uint8_t> bytes) {
    code.insert(code.end(), bytes);
}
void packet(std::vector<std::uint8_t>& code, const Packet& bytes) {
    const auto joyp = [&](std::uint8_t value) { append(code,{0x3e,value,0xe0,0}); };
    joyp(0x30); joyp(0); joyp(0x30);
    for (auto value: bytes) for (unsigned bit=0;bit<8;++bit) {
        joyp((value & (1U<<bit)) ? 0x10 : 0x20); joyp(0x30);
    }
    joyp(0x20); joyp(0x30); // stop bit, complete the actual JOYP transaction
}
gameboy::SgbHostConfig config(gameboy::HardwareModel model, const std::vector<Packet>& packets,
                              bool combined=false, bool bundled_boot=false) {
    gameboy::SgbHostConfig c;
    c.model=model; c.combined_audio=combined;
    c.program_rom=gameboy::firmware::sgb_prototype_rom();
    c.game_rom.resize(32768); c.game_rom[0x146]=3;
    c.game_rom[0x100]=0xc3; c.game_rom[0x101]=0x50; c.game_rom[0x102]=1;
    if (bundled_boot) c.gb_boot_rom = model==gameboy::HardwareModel::sgb
        ? gameboy::sgb_boot_rom() : gameboy::sgb2_boot_rom();
    else {
        // Original minimal GB bootstrap, leaves GB sound silent.
        const std::uint8_t boot[]{0x3e,0x91,0xe0,0x40,0xc3,0,1};
        std::copy(std::begin(boot),std::end(boot),c.gb_boot_rom.begin());
    }
    std::vector<std::uint8_t> code;
    for (const auto& p: packets) {
        // Wait a full LCD frame between commands: low and high LY thresholds.
        append(code,{0xf0,0x44,0xfe,0x90,0x30,0xfa,0xf0,0x44,0xfe,0x90,0x38,0xfa});
        packet(code,p);
    }
    append(code,{0x18,0xfe});
    std::copy(code.begin(),code.end(),c.game_rom.begin()+0x150);
    return c;
}
std::vector<Host::StereoSample> advance(Host& h, std::uint64_t clocks, bool restore=false) {
    const auto target=h.cpu().timing().clocks()+clocks;
    std::vector<Host::StereoSample> out;
    Host::StereoSample sample;
    unsigned steps=0;
    while(h.cpu().timing().clocks()<target) {
        if(!h.step()) {
            const auto f=h.fault();
            throw std::runtime_error("host fault status="+std::to_string(unsigned(h.status()))+
                " pc="+std::to_string(f.pc)+" opcode="+std::to_string(f.opcode)+
                " address="+std::to_string(f.address));
        }
        while(h.pop_sample(sample)) out.push_back(sample);
        if(restore && (++steps%1777)==0) {
            const auto saved=h.save_state();
            require(h.load_state(saved),"host snapshot restores during real firmware execution");
            require(h.save_state()==saved,"snapshot exact roundtrip");
        }
    }
    return out;
}
bool equal(const std::vector<Host::StereoSample>& a,const std::vector<Host::StereoSample>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](auto x,auto y){
        return x.left==y.left && x.right==y.right;
    });
}
bool audible(const std::vector<Host::StereoSample>& pcm) {
    return std::any_of(pcm.begin(),pcm.end(),[](auto x){return x.left>100 || x.left < -100;});
}
void startup(gameboy::HardwareModel model) {
    auto c=config(model,{},false,true);
    Host upload(c), uploaded(c);
    (void)advance(upload,120'000);
    require(upload.cpu().debug_wram_byte(0x20)==0 && upload.cpu().registers().x>0 &&
            upload.cpu().registers().x<gameboy::firmware::sgb_prototype_spc.size(),
            "checkpoint lies within genuine IPL upload before GB release");
    require(uploaded.load_state(upload.save_state()),"mid-upload cross-instance restore");
    require(equal(advance(upload,200'000),advance(uploaded,200'000)),"mid-upload continuation PCM");
    require(upload.save_state()==uploaded.save_state(),"mid-upload continuation state");
    Host h(c), other(c);
    const auto pcm=advance(h,12'000'000,true);
    require(h.cpu().debug_wram_byte(0x20)==1,"SPC ready precedes GB release");
    require(h.icd().packets_delivered()>=6,"bundled GB bootstrap packets consumed without originals");
    require(!audible(pcm),"untriggered original sound bank stays silent");
    require(other.load_state(h.save_state()),"whole host restores across instances");
    require(equal(advance(h,500'000),advance(other,500'000)),"cross-instance continuation PCM");
    require(h.save_state()==other.save_state(),"cross-instance continuation state");
    h.reset();
    require(equal(pcm,advance(h,12'000'000)),"cold reset reproduces bundled boot and silence");
}
void sound(gameboy::HardwareModel model, bool combined) {
    const Packet a{0x41,1}, b{0x41,0,1}, stop{0x41,0x80,0x80};
    auto c=config(model,{a,b,stop},combined);
    Host h(c), scalar(c);
    scalar.debug_set_apu_batch_enabled(false);
    // Compare ordinary scalar and optimized playback including upload/commands.
    const auto pcm=advance(h,2'000'000,true), expected=advance(scalar,2'000'000);
    require(equal(pcm,expected),"scalar/optimized prototype PCM agree");
    require(h.save_state()==scalar.save_state(),"scalar/optimized prototype state agree");
    require(h.icd().sound_packets_delivered()==3,"all start/stop SOUND packets delivered");
    require(h.cpu().debug_wram_byte(0x20)==1,"supported SOUND leaves firmware running");
    require(audible(pcm),"SOUND runs original SPC code and produces DSP audio");
    require(!audible(advance(h,1'000'000)),"stop command silences both voices after release tail");
    h.reset();
    require(equal(pcm,advance(h,2'000'000)),"reset reproduces original sound playback");
}
void instruments(gameboy::HardwareModel model) {
    Host a(config(model,{Packet{0x41,1}})), b(config(model,{Packet{0x41,0,1}}));
    const auto left=advance(a,1'200'000), right=advance(b,1'200'000);
    require(audible(left) && audible(right),"each SPC voice sounds independently");
    require(!equal(left,right),"two authored instruments have distinct PCM");
    auto incompatible=config(model,{});
    incompatible.program_rom[0x1000]^=1;
    Host changed(incompatible);
    require(!changed.load_state(a.save_state()),"different program-image identity rejected");
}
void unsupported(gameboy::HardwareModel model) {
    for (const Packet p: {Packet{0x49}, Packet{0x41,1,0,1}, Packet{0x41,1,0,0,1}, Packet{0x42,1}, Packet{0x41,2}, Packet{0x41,0,2}}) {
        Host h(config(model,{Packet{0x41,1,1},p}));
        (void)advance(h,2'000'000,true);
        require(h.cpu().debug_wram_byte(0x20)==0xff,"unsupported audio command halts prototype");
        require(h.cpu().debug_wram_byte(0x21)==p[0],"unsupported header retained for diagnosis");
        require(!audible(advance(h,500'000)),"unsupported audio silences both voices");
    }
}
}
int main() {
    try {
        for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
            startup(model); instruments(model); sound(model,false); sound(model,true); unsupported(model);
        }
        std::cout<<"Original SGB1/SGB2 firmware: upload, boot packets, two voices, stop, unsupported audio, reset, restore, scalar/combined playback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
