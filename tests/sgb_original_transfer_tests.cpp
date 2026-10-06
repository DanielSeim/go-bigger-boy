#include "gameboy/sgb_host.hpp"
#include "gameboy/sgb_sound.hpp"
#include "../firmware/sgb/prototype_image.hpp"
#include <iostream>
#include <fstream>
#include <cmath>
#include <stdexcept>

namespace {
using Host=gameboy::SgbHost;
using Payload=gameboy::SgbSoundTransfer::Payload;
using Packet=std::array<std::uint8_t,16>;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
void append(std::vector<std::uint8_t>& bytes,std::initializer_list<std::uint8_t> data) {
    bytes.insert(bytes.end(),data);
}
void packet(std::vector<std::uint8_t>& code,Packet p) {
    const auto pulse=[&](std::uint8_t v){append(code,{0x3e,v,0xe0,0});};
    pulse(0x30);pulse(0);pulse(0x30);
    for(auto byte:p) for(unsigned b=0;b<8;++b) {pulse(byte&(1U<<b)?0x10:0x20);pulse(0x30);}
    pulse(0x20);pulse(0x30);
}
gameboy::SgbHostConfig config(gameboy::HardwareModel model,const Payload& payload,unsigned followup=0,bool lcd=true,bool late=false) {
    gameboy::SgbHostConfig c;
    c.model=model;c.program_rom=gameboy::firmware::sgb_prototype_rom();
    c.game_rom.resize(32768);c.game_rom[0x146]=3;
    const std::uint8_t boot[]{0xc3,0,1};
    std::copy(std::begin(boot),std::end(boot),c.gb_boot_rom.begin());
    c.game_rom[0x100]=0xc3;c.game_rom[0x101]=0x50;c.game_rom[0x102]=1;
    // Actual GB code prepares VRAM while LCD is disabled, copies all 4096
    // bytes and lays out consecutive unsigned tiles across the 20-wide screen.
    std::vector<std::uint8_t> code{0xf3,0xaf,0xe0,0x40,0x11,0,0x40,0x21,0,0x80,
        0x01,0,0x10,0x1a,0x22,0x13,0x0b,0x78,0xb1,0x20,0xf8};
    for(unsigned row=0,index=0;index<256;++row) {
        const auto address=0x9800+32*row;
        append(code,{0x21,static_cast<std::uint8_t>(address),static_cast<std::uint8_t>(address>>8)});
        for(unsigned x=0;x<20 && index<256;++x,++index)
            append(code,{0x3e,static_cast<std::uint8_t>(index),0x22});
    }
    append(code,{0x3e,0xe4,0xe0,0x47});
    if(lcd) append(code,{0x3e,0x91,0xe0,0x40});
    packet(code,Packet{0x49});
    if(late) {
        // Update the final tile only after the command, in the following
        // VBlank. A stale/current-frame capture would retain the old bytes.
        append(code,{0xf0,0x44,0xfe,0x90,0x38,0xfa,0x21,0xf0,0x8f});
        for(unsigned i=0;i<16;++i) append(code,{0x3e,static_cast<std::uint8_t>(0xa0+i),0x22});
    }
    if(followup) {
        // Wait eight frames, allowing the initial upload and jump to complete.
        for(unsigned n=0;n<8;++n) append(code,{0xf0,0x44,0xfe,0x90,0x30,0xfa,
            0xf0,0x44,0xfe,0x90,0x38,0xfa});
        packet(code,followup==1 ? Packet{0x41,1} :
            followup==3 ? Packet{0x41,1,0,1} :
            followup==4 ? Packet{0x41,2} :
            followup==5 ? Packet{0x41,0,0,0,1} :
            followup==6 ? Packet{0x41,0,0,0,2} :
            followup==7 ? Packet{0x41,4} :
            followup==8 ? Packet{0x41,3,3,0,2} :
            followup==9 ? Packet{0x41,1,1,0,1} :
            followup==10 || followup==11 || followup==12 ? Packet{0x41,0,0,0,1} : Packet{0x49});
        if(followup==9 || followup==10 || followup==11 || followup==12) {
            for(unsigned n=0;n<(followup==12 ? 16U : 8U);++n) append(code,{0xf0,0x44,0xfe,0x90,0x30,0xfa,
                0xf0,0x44,0xfe,0x90,0x38,0xfa});
            packet(code,followup==10 ? Packet{0x41,0,0,0,0x80} :
                followup==11 || followup==12 ? Packet{0x41,0,0,0,1} : Packet{0x41,0,0,0,2});
        }
    }
    append(code,{0x18,0xfe});
    require(code.size()+0x150<0x4000,"original fixture code does not overlap transfer data");
    std::copy(code.begin(),code.end(),c.game_rom.begin()+0x150);
    std::copy(payload.begin(),payload.end(),c.game_rom.begin()+0x4000);
    return c;
}
std::vector<Host::StereoSample> advance(Host& h,std::uint64_t clocks,bool restore=false) {
    const auto target=h.cpu().timing().clocks()+clocks;
    std::vector<Host::StereoSample> pcm;Host::StereoSample sample;unsigned steps=0;
    while(h.cpu().timing().clocks()<target) {
        if(!h.step()) throw std::runtime_error("host fault status="+std::to_string(unsigned(h.status()))+
            " pc="+std::to_string(h.fault().pc)+" address="+std::to_string(h.fault().address));
        while(h.pop_sample(sample)) pcm.push_back(sample);
        if(restore && ++steps%1777==0) {
            const auto saved=h.save_state();require(h.load_state(saved),"transfer snapshot load");
            require(h.save_state()==saved,"transfer snapshot exact roundtrip");
        }
    }
    return pcm;
}
bool equal(const std::vector<Host::StereoSample>& a,const std::vector<Host::StereoSample>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](auto x,auto y){
        return x.left==y.left && x.right==y.right;
    });
}
void latched(const Host& h,const Payload& payload) {
    for(unsigned i=0;i<payload.size();++i)
        if(h.cpu().debug_wram_byte(0x1000+i)!=payload[i])
            throw std::runtime_error("ICD latch mismatch at "+std::to_string(i)+
                " expected="+std::to_string(payload[i])+" actual="+
                std::to_string(h.cpu().debug_wram_byte(0x1000+i)));
    require(h.cpu().dma_destination_count(0x80)==13,"thirteen real ICD-to-WRAM row DMAs");
}
std::vector<std::uint8_t> diagnostic() {
    // Uploaded original SPC code disables the IPL overlay and hashes ALL
    // transferred data, including $FFFF, through ordinary SPC RAM reads.
    std::vector<std::uint8_t> code{0x8f,0,0xf1};
    const auto hash=[&](std::uint8_t opcode,std::uint8_t port) {
        append(code,{0xe8,0});
        for(const auto block:std::vector<std::pair<unsigned,unsigned>>{{0x20f3,256},{0x21f3,256},{0x22f3,254},{0xfff0,16}}) {
            append(code,{0xcd,0});
            const auto loop=code.size();
            if(opcode==0x95) code.push_back(0x60); // clear carry for sum modulo 256
            else append(code,{0xc4,0x12}); // save XOR accumulator in direct RAM
            append(code,{opcode,static_cast<std::uint8_t>(block.first),static_cast<std::uint8_t>(block.first>>8)});
            if(opcode==0xf5) append(code,{0x44,0x12});
            code.push_back(0x3d);
            if(block.second!=256) append(code,{0xc8,static_cast<std::uint8_t>(block.second)});
            code.push_back(0xd0);code.push_back(static_cast<std::uint8_t>(static_cast<int>(loop)-static_cast<int>(code.size())-1));
        }
        append(code,{0xc4,port});
    };
    hash(0x95,0xf6);hash(0xf5,0xf7);
    // Sound the preserved authored square instrument, proving actual handoff.
    append(code,{0x8f,0x67,0xf2,0x8f,0x60,0xf3,0x8f,0x5c,0xf2,0x8f,0,0xf3,0x8f,0x4c,0xf2,0x8f,0x40,0xf3,0x2f,0xfe});
    return code;
}
Payload valid(std::uint8_t& sum,std::uint8_t& xor_sum) {
    Payload p{};
    for(unsigned i=0;i<p.size();++i) p[i]=static_cast<std::uint8_t>(i*73+i/7+19);
    unsigned offset=0;
    const auto word=[&](unsigned value){p[offset++]=value;p[offset++]=value>>8;};
    auto code=diagnostic();word(code.size());word(0x0800);
    for(auto byte:code) p[offset++]=byte;
    word(766);word(0x20f3); // N+2 wraps to zero; requires nonzero block token
    for(unsigned i=0;i<766;++i) {auto b=static_cast<std::uint8_t>(i*37+i/11+31);p[offset++]=b;sum+=b;xor_sum^=b;}
    word(16);word(0xfff0);
    for(unsigned i=0;i<16;++i) {auto b=static_cast<std::uint8_t>(i*61+9);p[offset++]=b;sum+=b;xor_sum^=b;}
    word(0);word(0x0800);
    require(gameboy::SgbSoundTransfer::parse(p).valid(),"valid fixture matches independent SOU_TRN parser");
    return p;
}
void checkpoints(const gameboy::SgbHostConfig& cfg) {
    Host a(cfg),b(cfg);
    const auto until=[&](auto predicate) {
        while(!predicate() && a.cpu().timing().clocks()<10'000'000) (void)advance(a,500);
        require(predicate(),"reach transfer checkpoint before handoff");
        require(b.load_state(a.save_state()),"cross-instance transfer checkpoint");
        require(equal(advance(a,5000),advance(b,5000)),"checkpoint continuation PCM");
        require(a.save_state()==b.save_state(),"checkpoint continuation full state");
    };
    until([&]{return a.cpu().debug_wram_byte(0x20)==3 && a.cpu().dma_start_count()>0;});
    until([&]{return a.cpu().debug_wram_byte(0x20)==4 &&
        a.cpu().registers().x>10 && a.cpu().registers().x<100;});
}
void upload(gameboy::HardwareModel model) {
    std::uint8_t sum=0,xor_sum=0;const auto p=valid(sum,xor_sum);
    auto c=config(model,p);checkpoints(c);Host h(c),scalar(c),restored(c);
    scalar.debug_set_apu_batch_enabled(false);
    const auto pcm=advance(h,10'000'000,true),reference=advance(scalar,10'000'000);
    latched(h,p);
    require(h.cpu().debug_wram_byte(0x20)==2,"uploaded driver owns APU");
    require(h.cpu().debug_wram_byte(0x26)==1 && h.cpu().debug_wram_byte(0x27)==0,"one valid upload and jump");
    require(h.cpu().debug_wram_byte(0x28)==sum && h.cpu().debug_wram_byte(0x29)==xor_sum,
            "uploaded SPC program verifies all transferred data including upper-RAM boundary");
    require(std::any_of(pcm.begin(),pcm.end(),[](auto s){return s.left>100 || s.left < -100;}),"uploaded code renders DSP audio");
    require(equal(pcm,reference) && h.save_state()==scalar.save_state(),"scalar upload and playback parity");
    require(restored.load_state(h.save_state()),"cross-instance post-handoff restore");
    require(equal(advance(h,200'000),advance(restored,200'000)) && h.save_state()==restored.save_state(),"post-handoff continuation");
    h.reset();require(equal(pcm,advance(h,10'000'000)),"reset repeats physical capture/upload/audio");
    Host next_frame(config(model,p,0,true,true));(void)advance(next_frame,10'000'000);
    auto changed=p;
    for(unsigned i=0;i<16;++i) changed[0xff0+i]=0xa0+i;
    latched(next_frame,changed);
    require(next_frame.cpu().debug_wram_byte(0x26)==1,"next-frame update remains a valid upload");
    // Stop assumptions at the ownership boundary: unsupported SOUND must not
    // send our mailbox token or stop codes to an unrelated uploaded program.
    for(unsigned command:{1U,2U}) {
        Host external(config(model,p,command));(void)advance(external,10'000'000);
        require(external.cpu().debug_wram_byte(0x20)==0xff &&
                external.cpu().debug_wram_byte(0x21)==(command==1 ? 0x41 : 0x49),
                "external SOUND/SOU_TRN rejected explicitly");
        require(external.cpu().debug_wram_byte(0x28)==sum && external.cpu().debug_wram_byte(0x29)==xor_sum,"external output ports remain untouched");
    }
}
Payload program_payload(const std::vector<std::uint8_t>& program,unsigned destination,unsigned entry=0) {
    Payload p{};unsigned offset=0;
    const auto word=[&](unsigned v){p[offset++]=v;p[offset++]=v>>8;};
    require(program.size()+8<=p.size(),"uploaded test driver fits transfer screen");
    if(!program.empty()) {
        word(program.size());word(destination);
        for(auto b:program) p[offset++]=b;
    }
    word(0);word(entry ? entry : destination);return p;
}
void unsupported_score_restart(gameboy::HardwareModel model) {
    // Independently authored score data, never vendor/title bytes. The reserved
    // restart must withdraw readiness rather than pretending to decode it.
    std::vector<std::uint8_t> bank(64);
    bank[0x10]=0x20;bank[0x11]=0x2b; // phrase -> eight-track pattern
    bank[0x20]=0x30;bank[0x21]=0x2b; // channel 0 -> original note
    bank[0x30]=4;bank[0x31]=0x98;
    const auto payload=program_payload(bank,0x2b00,0x0400);
    const auto c=config(model,payload);
    Host h(c),scalar(c),restored(c);scalar.debug_set_apu_batch_enabled(false);
    const auto pcm=advance(h,8'000'000),expected=advance(scalar,8'000'000);
    latched(h,payload);
    require(h.cpu().debug_wram_byte(0x20)==2 && h.cpu().debug_wram_byte(0x24)==1,
            "unsupported score restart remains external");
    require(h.cpu().debug_wram_byte(0x25)==1 && h.cpu().debug_wram_byte(0x26)==1 &&
            h.cpu().debug_wram_byte(0x2a)==1 && h.cpu().debug_wram_byte(0x27)==0,
            "score capture/handoff completes without false adoption or loader error");
    require(h.cpu().debug_wram_byte(0x28)==0xe1 && h.cpu().debug_wram_byte(0x29)==0,
            "real SPC restart publishes unsupported-score diagnostic and clears signature");
    require(equal(pcm,expected) && h.save_state()==scalar.save_state(),"score restart scalar parity");
    require(restored.load_state(h.save_state()),"restore unsupported score restart");
    const auto quiet=advance(h,200'000),continuation=advance(restored,200'000);
    require(equal(quiet,continuation) && h.save_state()==restored.save_state(),"score restart restored continuation");
    require(std::all_of(quiet.begin(),quiet.end(),[](auto s){return s.left==0 && s.right==0;}),
            "unsupported score restart leaves DSP silent");
    h.reset();require(equal(pcm,advance(h,8'000'000)),"reset reproduces score restart");
    Host rejected(config(model,payload,1));(void)advance(rejected,10'000'000);
    require(rejected.cpu().debug_wram_byte(0x20)==0xff && rejected.cpu().debug_wram_byte(0x21)==0x41 &&
            rejected.cpu().debug_wram_byte(0x2a)==1 && rejected.cpu().debug_wram_byte(0x28)==0xe1,
            "SOUND after unimplemented restart halts explicitly without reacquiring ownership");
    Host uploading(c),inflight(c);Host::StereoSample sample;
    while(uploading.cpu().debug_wram_byte(0x20)!=4 && uploading.cpu().timing().clocks()<9'000'000) {
        require(uploading.step(),"reach score upload checkpoint");
        while(uploading.pop_sample(sample)) {}
    }
    require(uploading.cpu().debug_wram_byte(0x20)==4,"snapshot during score upload");
    require(inflight.load_state(uploading.save_state()),"restore in-flight score upload");
    require(equal(advance(uploading,2'000'000),advance(inflight,2'000'000)) &&
            uploading.save_state()==inflight.save_state(),"score handoff continues identically across restore");
    require(uploading.cpu().debug_wram_byte(0x28)==0xe1,"restored upload reaches reserved score entry");
}
gameboy::SgbHostConfig sequence(gameboy::HardwareModel model,const std::vector<Payload>& payloads,bool wrap=false) {
    auto c=config(model,payloads.front());
    std::vector<std::uint8_t> code;
    const auto frames=[&](unsigned count) {
        for(unsigned i=0;i<count;++i) append(code,{0xf0,0x44,0xfe,0x90,0x30,0xfa,
            0xf0,0x44,0xfe,0x90,0x38,0xfa});
    };
    for(unsigned stage=0;stage<payloads.size();++stage) {
        const auto source=0x4000+stage*0x1000;
        append(code,{0xf3,0xaf,0xe0,0x40,0x11,static_cast<std::uint8_t>(source),
            static_cast<std::uint8_t>(source>>8),0x21,0,0x80,0x01,0,0x10,
            0x1a,0x22,0x13,0x0b,0x78,0xb1,0x20,0xf8});
        for(unsigned row=0,index=0;index<256;++row) {
            auto address=0x9800+row*32;
            append(code,{0x21,static_cast<std::uint8_t>(address),static_cast<std::uint8_t>(address>>8)});
            for(unsigned x=0;x<20 && index<256;++x,++index)
                append(code,{0x3e,static_cast<std::uint8_t>(index),0x22});
        }
        append(code,{0x3e,0xe4,0xe0,0x47,0x3e,0x91,0xe0,0x40});
        packet(code,Packet{0x49});frames(4);
        packet(code,Packet{0x41,1,1});frames(1);
        packet(code,Packet{0x41,0x80,0x80});
        std::copy(payloads[stage].begin(),payloads[stage].end(),c.game_rom.begin()+source);
    }
    if(wrap) {
        append(code,{0x06,0}); // B=0 means 256 repetitions, packet code preserves B.
        const auto loop=0x150+code.size();
        packet(code,Packet{0x41});
        append(code,{0x05,0xc2,static_cast<std::uint8_t>(loop),static_cast<std::uint8_t>(loop>>8)});
    }
    append(code,{0x18,0xfe});
    require(code.size()+0x150<0x4000,"sequence code fits before payloads");
    std::copy(code.begin(),code.end(),c.game_rom.begin()+0x150);return c;
}
void interoperability(gameboy::HardwareModel model) {
    // The original position-independent driver is genuinely uploaded to two
    // distinct addresses. 766-byte blocks end with jump token 1, which would
    // alias the first SOUND token without the explicit zero-arm handshake.
    const auto first=gameboy::firmware::sgb_prototype_spc.begin()+gameboy::firmware::sgb_prototype_driver_offset;
    std::vector<std::uint8_t> driver(first,first+gameboy::firmware::sgb_prototype_driver_size);
    driver.resize(766); // preserve the token-alias regression case without uploading entry stubs
    const std::vector<Payload> payloads{program_payload(driver,0x0800),
        program_payload(driver,0x0c00),program_payload({},0x0200)};
    auto c=sequence(model,payloads);Host h(c),scalar(c),peer(c);
    scalar.debug_set_apu_batch_enabled(false);
    const auto pcm=advance(h,14'000'000),expected=advance(scalar,14'000'000);
    require(h.cpu().debug_wram_byte(0x20)==1 && h.cpu().debug_wram_byte(0x24)==0,"compatible uploaded driver reacquires mailbox ownership");
    require(h.cpu().debug_wram_byte(0x25)==3 && h.cpu().debug_wram_byte(0x26)==3 &&
            h.cpu().debug_wram_byte(0x2a)==4,"three transfers, two relocated drivers and resident restart adopted");
    require(h.icd().sound_packets_delivered()==6 && h.cpu().debug_wram_byte(0x23)==4,"SOUND starts and stops after every handoff");
    require(h.cpu().dma_destination_count(0x80)==39,"all three complete transfer screens captured");
    for(unsigned i=0;i<4096;++i) require(h.cpu().debug_wram_byte(0x1000+i)==payloads.back()[i],"final repeated-transfer latch exact");
    require(std::any_of(pcm.begin(),pcm.end(),[](auto s){return s.left>100 || s.left < -100;}),"compatible uploaded drivers sound real instruments");
    require(equal(pcm,expected) && h.save_state()==scalar.save_state(),"repeated transfer scalar parity");
    require(peer.load_state(h.save_state()),"repeated transfer cross-instance restore");
    require(equal(advance(h,200'000),advance(peer,200'000)) && h.save_state()==peer.save_state(),"repeated transfer restored continuation");
    h.reset();require(equal(pcm,advance(h,14'000'000)),"reset repeats multiple driver adoptions and SOUND");
    const auto quiet=advance(h,500'000);
    require(std::all_of(quiet.begin(),quiet.end(),[](auto s){return s.left==0 && s.right==0;}),"stop remains silent after reacquisition");
    Host negotiating(c),restored(c);Host::StereoSample sample;
    while(!(negotiating.cpu().debug_wram_byte(0x20)==5 && negotiating.cpu().debug_wram_byte(0x26)==1)
          && negotiating.cpu().timing().clocks()<10'000'000) {
        require(negotiating.step(),"reach uploaded-driver arm gate");
        while(negotiating.pop_sample(sample)) {}
    }
    require(negotiating.cpu().debug_wram_byte(0x20)==5,"snapshot while driver is being armed");
    require(restored.load_state(negotiating.save_state()),"restore in-flight arm handshake");
    require(equal(advance(negotiating,20'000),advance(restored,20'000)) &&
            negotiating.save_state()==restored.save_state(),"in-flight driver adoption continues identically");
    if(model==gameboy::HardwareModel::sgb2) {
        Host wrapped(sequence(model,{payloads.front()},true));(void)advance(wrapped,15'000'000);
        require(wrapped.cpu().debug_wram_byte(0x20)==1 && wrapped.cpu().debug_wram_byte(0x27)==0,
                "ordinary mailbox token zero is valid after counter rollover");
        require(wrapped.icd().sound_packets_delivered()==258 && wrapped.cpu().debug_wram_byte(0x23)==4,
                "256 further SOUND packets cross v2 token rollover exactly twice");
    }
}
unsigned crossings(const std::vector<Host::StereoSample>& pcm,bool right=false) {
    unsigned count=0;
    for(unsigned i=1;i<pcm.size();++i) if((right ? pcm[i-1].right : pcm[i-1].left)<0 && (right ? pcm[i].right : pcm[i].left)>=0) ++count;
    return count;
}
void ready_score(Host& h) {
    Host::StereoSample sample;
    while((h.icd().sound_packets_delivered()!=1 || h.cpu().debug_wram_byte(0x23)!=2)
          && h.cpu().timing().clocks()<12'000'000) {
        require(h.step(),"reach score command after data-only handoff");
        while(h.pop_sample(sample)) {}
    }
    require(h.icd().sound_packets_delivered()==1 && h.cpu().debug_wram_byte(0x23)==2 &&
            h.cpu().debug_wram_byte(0x20)==1,"uploaded score selected after v3 arm and acknowledgment");
}
void resident_score(gameboy::HardwareModel model,const Payload& p) {
    for(bool combined:{false,true}) {
        auto c=config(model,p,5);c.combined_audio=combined;
        Host h(c),scalar(c),peer(c);scalar.debug_set_apu_batch_enabled(false);
        const auto boot=advance(h,10'000'000),reference=advance(scalar,10'000'000);
        require(equal(boot,reference) && h.save_state()==scalar.save_state(),"resident score native/combined scalar parity");
        if(!(h.cpu().debug_wram_byte(0x2b)==0xc5 && h.cpu().debug_wram_byte(0x2a)==2 &&
                h.cpu().debug_wram_byte(0x20)==1 && h.cpu().debug_wram_byte(0x23)==2))
            throw std::runtime_error("resident readiness: state="+std::to_string(h.cpu().debug_wram_byte(0x20))+
                " version="+std::to_string(h.cpu().debug_wram_byte(0x2b))+
                " diagnostic="+std::to_string(h.cpu().debug_wram_byte(0x28))+
                " adoptions="+std::to_string(h.cpu().debug_wram_byte(0x2a))+
                " token="+std::to_string(h.cpu().debug_wram_byte(0x23)));
        require(peer.load_state(h.save_state()),"restore resident score while active");
        require(equal(advance(h,1'000'000),advance(peer,1'000'000)) && h.save_state()==peer.save_state(),
                "resident note/countdown and DSP continuation restore exactly");
        h.reset();require(equal(boot,advance(h,10'000'000)),"resident score reset repeats upload and playback");
        Host measured(c);Host::StereoSample sample;
        while((measured.icd().sound_packets_delivered()!=1 || measured.cpu().debug_wram_byte(0x23)!=2)
              && measured.cpu().timing().clocks()<12'000'000) {
            require(measured.step(),"reach resident score first note");while(measured.pop_sample(sample)) {}
        }
        require(measured.cpu().debug_wram_byte(0x23)==2,"resident score start acknowledged");
        (void)advance(measured,100'000);
        const auto first=crossings(advance(measured,1'000'000));
        (void)advance(measured,1'800'000);
        const auto octave=crossings(advance(measured,1'000'000));
        require(first>5 && std::abs(double(octave)/first-2)<0.18,"uploaded octave notes render real DSP pitch ratio");
        (void)advance(measured,2'000'000);
        const auto tied=crossings(advance(measured,1'000'000));
        require(std::abs(double(tied)/octave-1)<0.12,"tie keeps uploaded pitch for another duration");
        (void)advance(measured,1'600'000);
        const auto rest=advance(measured,1'000'000);
        require(std::all_of(rest.begin(),rest.end(),[](auto s){return s.left==0 && s.right==0;}),"uploaded rest keys off music voice");
        (void)advance(measured,1'800'000);
        require(crossings(advance(measured,1'000'000))>5,"note after rest resumes playback");
        (void)advance(measured,4'000'000);
        const auto ended=advance(measured,200'000);
        require(std::all_of(ended.begin(),ended.end(),[](auto s){return s.left==0 && s.right==0;}),"finite uploaded score ends silently");
    }
    for(unsigned followup:{6U,8U}) {
        Host invalid(config(model,p,followup));(void)advance(invalid,10'000'000);
        require(invalid.cpu().debug_wram_byte(0x20)==0xff && invalid.cpu().debug_wram_byte(0x2b)==0xc5,
                "v5 rejects score two and silences all voices");
        const auto quiet=advance(invalid,500'000);
        require(std::all_of(quiet.begin(),quiet.end(),[](auto s){return s.left==0 && s.right==0;}),"invalid v5 command stays silent");
    }
    std::vector<Payload> invalid;
    auto changed=p;changed[4]='X';invalid.push_back(changed); // header
    changed=p;changed[8]=0xff;invalid.push_back(changed); // missing terminal at declared end
    changed=p;changed[4+0x12]=1;invalid.push_back(changed); // other channel
    changed=p;changed[4+0x21]=0xe0;invalid.push_back(changed); // unsupported control
    changed=p;changed[4+0x21]=0xc8;invalid.push_back(changed); // tie before note
    for(const auto& payload:invalid) {
        Host rejected(config(model,payload));(void)advance(rejected,9'000'000);
        require(rejected.cpu().debug_wram_byte(0x24)==1 && rejected.cpu().debug_wram_byte(0x2a)==1 &&
                rejected.cpu().debug_wram_byte(0x28)>=0xe1 && rejected.cpu().debug_wram_byte(0x29)==0,
                "bad bank rejected before v5 readiness without false adoption");
    }
    Host repeated(sequence(model,{p,p,program_payload({},0x0200)}));
    (void)advance(repeated,16'000'000);
    require(repeated.cpu().debug_wram_byte(0x26)==3 && repeated.cpu().debug_wram_byte(0x2a)==4 &&
            repeated.cpu().debug_wram_byte(0x2b)==0xc4 && repeated.cpu().debug_wram_byte(0x20)==1,
            "repeated v5 bank uploads rearm and legacy restart returns to v4");
}
double channel_rms(const std::vector<Host::StereoSample>& pcm,bool right=false) {
    double sum=0;
    for(auto sample:pcm) {const double level=right ? sample.right : sample.left;sum+=level*level;}
    return std::sqrt(sum/pcm.size());
}
void resident_ready(Host& h,std::uint8_t version=0xc6) {
    Host::StereoSample sample;
    while((h.icd().sound_packets_delivered()!=1 || h.cpu().debug_wram_byte(0x23)!=2)
          && h.cpu().timing().clocks()<12'000'000) {
        require(h.step(),"reach controlled score start");while(h.pop_sample(sample)) {}
    }
    require(h.cpu().debug_wram_byte(0x2b)==version && h.cpu().debug_wram_byte(0x23)==2,
            "score validation advertises expected version and arms before SOUND");
}
void resident_controls(gameboy::HardwareModel model,const Payload& p) {
    for(bool combined:{false,true}) {
        auto c=config(model,p,5);c.combined_audio=combined;
        Host h(c),scalar(c),peer(c);scalar.debug_set_apu_batch_enabled(false);
        const auto boot=advance(h,10'000'000),reference=advance(scalar,10'000'000);
        require(equal(boot,reference) && h.save_state()==scalar.save_state(),"controlled score scalar native/combined parity");
        require(h.cpu().debug_wram_byte(0x2b)==0xc6 && h.cpu().debug_wram_byte(0x2a)==2,
                "controlled score adopts v6 after validation");
        require(peer.load_state(h.save_state()),"restore control state during active score");
        require(equal(advance(h,3'000'000),advance(peer,3'000'000)) && h.save_state()==peer.save_state(),
                "gain/pan/instrument and timer state restore through control changes");
        h.reset();require(equal(boot,advance(h,10'000'000)),"reset repeats controlled score upload and playback");
        Host measured(c);resident_ready(measured);(void)advance(measured,100'000);
        const auto full=advance(measured,1'000'000);
        (void)advance(measured,1'800'000);
        const auto half=advance(measured,1'000'000);
        const auto full_level=channel_rms(full),half_level=channel_rms(half);
        require(full_level>100 && std::abs(half_level/full_level-0.5)<0.04,"score volume halves real DSP amplitude");
        require(std::abs(channel_rms(full,true)/full_level-1)<0.02,"center pan gives equal stereo levels");
        Host snapshot(c);require(snapshot.load_state(measured.save_state()),"snapshot after volume change before pan");
        require(equal(advance(measured,2'000'000),advance(snapshot,2'000'000)) && measured.save_state()==snapshot.save_state(),
                "pending pan change continues identically across save/load");
        const auto left=advance(measured,1'000'000);
        require(channel_rms(left)>half_level*1.9 && channel_rms(left,true)==0,"hard left pan retains held tie and silences right channel");
        (void)advance(measured,1'600'000);
        const auto right=advance(measured,1'000'000);
        require(channel_rms(right)==0 && channel_rms(right,true)>half_level*1.9,"hard right pan reverses stereo routing");
        (void)advance(measured,1'800'000);
        const auto triangle=advance(measured,1'000'000);
        require(channel_rms(triangle)>100 && std::abs(channel_rms(triangle,true)/channel_rms(triangle)-1)<0.02,
                "instrument change and center pan resume audible stereo note");
    }
    std::vector<std::vector<Host::StereoSample>> instruments;
    for(unsigned source=0;source<4;++source) {
        std::vector<std::uint8_t> bank(p.begin()+4,p.begin()+36);
        const std::uint8_t track[]{0xe0,static_cast<std::uint8_t>(source),0xe1,10,0xed,64,32,0x8c,0};
        bank.insert(bank.end(),std::begin(track),std::end(track));bank[4]=bank.size();
        Host voice(config(model,program_payload(bank,0x2b00,0x0400),5));resident_ready(voice);
        (void)advance(voice,100'000);instruments.push_back(advance(voice,1'000'000));
        require(channel_rms(instruments.back())>100,"each authored score instrument sounds");
    }
    for(unsigned i=0;i<4;++i) for(unsigned j=0;j<i;++j)
        require(!equal(instruments[i],instruments[j]),"instrument command selects four distinct original BRR waveforms");
    std::vector<std::uint8_t> mute_bank(p.begin()+4,p.begin()+36);
    const std::uint8_t mute_track[]{0xed,0,32,0x8c,0};
    mute_bank.insert(mute_bank.end(),std::begin(mute_track),std::end(mute_track));mute_bank[4]=mute_bank.size();
    Host muted(config(model,program_payload(mute_bank,0x2b00,0x0400),5));resident_ready(muted);
    const auto silent=advance(muted,1'000'000);
    require(std::all_of(silent.begin(),silent.end(),[](auto s){return s.left==0 && s.right==0;}),"zero volume operand mutes an active timed note");
    Host bad_score(config(model,p,9));(void)advance(bad_score,14'000'000);
    require(bad_score.cpu().debug_wram_byte(0x20)==0xff && bad_score.cpu().debug_wram_byte(0x2b)==0xc6 &&
            bad_score.icd().sound_packets_delivered()==2,"v6 rejects score two after concurrent music/effect start");
    const auto stopped=advance(bad_score,500'000);
    require(std::all_of(stopped.begin(),stopped.end(),[](auto s){return s.left==0 && s.right==0;}),"v6 unsupported command silences music and both effect voices");
    std::vector<Payload> invalid;
    auto changed=p;changed[7]='1';invalid.push_back(changed); // old GBS1 rejects controls
    changed=p;changed[37]=4;invalid.push_back(changed); // source bound
    changed=p;changed[39]=21;invalid.push_back(changed); // pan bound
    changed=p;changed[41]=128;invalid.push_back(changed); // volume bound
    const unsigned size=p[0]|unsigned(p[1])<<8;
    changed=p;changed[4+size-1]=0xe0;invalid.push_back(changed); // missing operand/terminator
    changed=p;changed[36]=0xe3;invalid.push_back(changed); // unsupported modulation
    for(const auto& payload:invalid) {
        Host rejected(config(model,payload));(void)advance(rejected,9'000'000);
        require(rejected.cpu().debug_wram_byte(0x24)==1 && rejected.cpu().debug_wram_byte(0x2a)==1 &&
                rejected.cpu().debug_wram_byte(0x28)==0xe2 && rejected.cpu().debug_wram_byte(0x29)==0,
                "bad controls fail before readiness with explicit syntax diagnostic");
    }
    Host repeated(sequence(model,{p,p,program_payload({},0x0200)}));(void)advance(repeated,16'000'000);
    require(repeated.cpu().debug_wram_byte(0x26)==3 && repeated.cpu().debug_wram_byte(0x2a)==4 &&
            repeated.cpu().debug_wram_byte(0x2b)==0xc4,"repeated v6 banks rearm and legacy entry returns v4");
}
void resident_tracks(gameboy::HardwareModel model,const Payload& p) {
    for(bool combined:{false,true}) {
        auto c=config(model,p,5);c.combined_audio=combined;
        Host h(c),scalar(c),peer(c);scalar.debug_set_apu_batch_enabled(false);
        const auto boot=advance(h,12'000'000),reference=advance(scalar,12'000'000);
        require(equal(boot,reference) && h.save_state()==scalar.save_state(),"two-track native/combined scalar parity");
        require(h.cpu().debug_wram_byte(0x2b)==0xc7 && h.cpu().debug_wram_byte(0x2a)==2,
                "two complete tracks adopt v7 after validation");
        h.reset();require(equal(boot,advance(h,12'000'000)),"reset repeats two-track upload and playback");
        Host measured(c);resident_ready(measured,0xc7);(void)advance(measured,100'000);
        const auto initial=advance(measured,1'000'000);
        const auto low=crossings(initial),high=crossings(initial,true);
        if(!(low>5 && std::abs(double(high)/low-2)<0.2))
            throw std::runtime_error("two-track octave: left="+std::to_string(low)+" right="+std::to_string(high)+
                " rms="+std::to_string(channel_rms(initial))+"/"+std::to_string(channel_rms(initial,true)));
        (void)advance(measured,600'000);
        require(peer.load_state(measured.save_state()),"snapshot during left rest and right note");
        const auto rest=advance(measured,500'000);
        require(equal(rest,advance(peer,500'000)),"rest continuation restores exactly");
        require(channel_rms(rest)==0 && crossings(rest,true)>5,"left rest preserves right note");
        (void)advance(measured,700'000);(void)advance(peer,700'000);
        const auto resumed=advance(measured,600'000);
        require(equal(resumed,advance(peer,600'000)),"resumed note and other track tie restore exactly");
        require(crossings(resumed)>5 && std::abs(double(crossings(resumed,true))/high/0.6-1)<0.15,
                "left resumes while right holds its independent tie");
        require(std::abs(channel_rms(resumed)/channel_rms(initial)-0.5)<0.04 &&
                std::abs(channel_rms(resumed,true)/channel_rms(initial,true)-1)<0.04,
                "first track gain change leaves second track gain unchanged");
        (void)advance(measured,1'000'000);(void)advance(peer,1'000'000);
        const auto left_ended=advance(measured,600'000);
        require(equal(left_ended,advance(peer,600'000)),"first track end restores exactly");
        require(channel_rms(left_ended)==0 && crossings(left_ended,true)>5,"first track end leaves second track active");
        const auto continuation=advance(measured,600'000);
        require(equal(continuation,advance(peer,600'000)) && measured.save_state()==peer.save_state(),
                "both cursors/countdowns and DSP restore exactly through rest tie and first end");
        const auto changed=advance(measured,600'000);
        require(channel_rms(changed)==0 && std::abs(double(crossings(changed,true))/high/0.6-0.75)<0.15,
                "second track advances to its own later note after first ends");
        (void)advance(measured,3'000'000);
        const auto ended=advance(measured,200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"both finite tracks end silently");
    }
    for(unsigned followup:{9U,10U}) {
        Host stopped(config(model,p,followup));Host::StereoSample sample;
        while((stopped.icd().sound_packets_delivered()!=2 ||
               stopped.cpu().debug_wram_byte(0x20)!=(followup==9 ? 0xff : 1) ||
               stopped.cpu().debug_wram_byte(0x23)!=(followup==9 ? 3 : 4)) &&
              stopped.cpu().timing().clocks()<12'000'000) {
            require(stopped.step(),"reach completed stop before natural track end");
            while(stopped.pop_sample(sample)) {}
        }
        // Direct-gain effect envelopes can release for up to eight milliseconds.
        (void)advance(stopped,200'000);
        require(stopped.cpu().debug_wram_byte(0x2b)==0xc7 && stopped.icd().sound_packets_delivered()==2 &&
                stopped.cpu().debug_wram_byte(0x20)==(followup==9 ? 0xff : 1),
                "v7 rejects score two and accepts explicit music stop");
        const auto quiet=advance(stopped,500'000);
        require(channel_rms(quiet)==0 && channel_rms(quiet,true)==0,"stop silences both tracks and rejected command silences effects");
    }
    std::vector<Payload> invalid;
    const auto split=p[9],size=p[8];
    auto changed=p;changed[4+0x12]++;invalid.push_back(changed); // pointer differs from split
    changed=p;changed[9]=0x22;invalid.push_back(changed); // first track cannot fit event and terminator
    changed=p;changed[4+split+1]=4;invalid.push_back(changed); // second instrument out of range
    changed=p;changed[4+split+7]=0xc8;invalid.push_back(changed); // second track tie without own note
    changed=p;changed[4+size-1]=0xe0;invalid.push_back(changed); // truncated second track
    changed=p;changed[4+0x14]=1;invalid.push_back(changed); // third channel
    for(const auto& payload:invalid) {
        Host rejected(config(model,payload));(void)advance(rejected,9'000'000);
        require(rejected.cpu().debug_wram_byte(0x24)==1 && rejected.cpu().debug_wram_byte(0x2a)==1 &&
                rejected.cpu().debug_wram_byte(0x28)>=0xe1 && rejected.cpu().debug_wram_byte(0x29)==0,
                "malformed track or header rejected before any v7 readiness");
        const auto quiet=advance(rejected,200'000);
        require(channel_rms(quiet)==0 && channel_rms(quiet,true)==0,"rejected two-track bank stays silent");
    }
    Host repeated(sequence(model,{p,p,program_payload({},0x0200)}));(void)advance(repeated,18'000'000);
    require(repeated.cpu().debug_wram_byte(0x26)==3 && repeated.cpu().debug_wram_byte(0x2a)==4 &&
            repeated.cpu().debug_wram_byte(0x2b)==0xc4 && repeated.cpu().debug_wram_byte(0x20)==1,
            "repeated v7 banks rearm and legacy entry returns to v4");
}
void resident_phrases(gameboy::HardwareModel model,const Payload& p) {
    for(bool combined:{false,true}) {
        auto c=config(model,p,5);c.combined_audio=combined;
        Host h(c),scalar(c),peer(c);scalar.debug_set_apu_batch_enabled(false);
        const auto boot=advance(h,14'000'000),reference=advance(scalar,14'000'000);
        require(equal(boot,reference) && h.save_state()==scalar.save_state(),"finite phrase native/combined scalar parity");
        require(h.cpu().debug_wram_byte(0x2b)==0xc8 && h.cpu().debug_wram_byte(0x2a)==2,
                "complete finite phrase list validates before v8 adoption");
        h.reset();require(equal(boot,advance(h,14'000'000)),"reset repeats finite phrase upload and playback");
        Host measured(c);resident_ready(measured,0xc8);(void)advance(measured,100'000);
        const auto initial=advance(measured,1'000'000);
        const auto low=crossings(initial),high=crossings(initial,true);
        require(low>5 && std::abs(double(high)/low-2)<0.2,"first phrase renders independent octave voices");
        (void)advance(measured,600'000);
        require(peer.load_state(measured.save_state()),"snapshot after short track ends before phrase barrier");
        const auto paired=[&](std::uint64_t clocks) {
            const auto pcm=advance(measured,clocks);
            require(equal(pcm,advance(peer,clocks)) && measured.save_state()==peer.save_state(),
                    "phrase index barrier track defaults and DSP continue exactly after restore");
            return pcm;
        };
        const auto waiting=paired(600'000);
        require(channel_rms(waiting)==0 && crossings(waiting,true)>5,"phrase waits for longer track after first ends");
        (void)paired(700'000);
        const auto defaults=paired(700'000);
        require(channel_rms(defaults)>100 && std::abs(channel_rms(defaults,true)/channel_rms(defaults)-1)<0.02,
                "next phrase restores center pan on both channels");
        require(std::abs(channel_rms(defaults,true)/channel_rms(initial,true)-0.625)<0.04,
                "next phrase restores triangle instrument and direct gain 80");
        require(std::abs(double(crossings(defaults))/high/0.7-1)<0.15,"next phrase starts its own note and duration");
        (void)paired(500'000);
        const auto rest=paired(600'000);
        require(channel_rms(rest)==0 && channel_rms(rest,true)==0,"both second phrase tracks rest independently");
        (void)paired(800'000);
        const auto barrier=paired(1'000'000);
        require(channel_rms(barrier)==0 && channel_rms(barrier,true)==0,"silent longer rest prevents premature third phrase");
        (void)paired(500'000);
        const auto final=paired(1'000'000);
        require(std::abs(double(crossings(final))/low-1.5)<0.15 &&
                std::abs(double(crossings(final,true))/high-0.5)<0.1,"third phrase selects new independent pitches");
        require(std::abs(channel_rms(final)/channel_rms(initial)-2)<0.06 &&
                std::abs(channel_rms(final,true)/channel_rms(initial,true)-0.5)<0.04,
                "third phrase applies authored independent gain controls");
        (void)paired(2'000'000);
        const auto ended=paired(200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"finite phrase list ends without looping");
    }
    for(unsigned count:{1U,4U}) {
        std::vector<std::uint8_t> bank(32+16*count);
        bank[0]='G';bank[1]='B';bank[2]='S';bank[3]='4';bank[5]=count;
        for(unsigned pattern=0;pattern<count;++pattern) {
            const unsigned table=32+16*pattern;
            bank[8+2*pattern]=table;bank[9+2*pattern]=0x2b;
            for(unsigned channel=0;channel<2;++channel) {
                bank[table+2*channel]=bank.size();bank[table+2*channel+1]=0x2b;
                if(pattern+1<count) append(bank,{0xed,0,0}); // immediate control-only phrase
                else append(bank,{8,static_cast<std::uint8_t>(channel==0 ? 0x8c : 0xc9),0});
            }
        }
        bank[4]=bank.size();
        Host edge(config(model,program_payload(bank,0x2b00,0x0400),5));resident_ready(edge,0xc8);
        (void)advance(edge,100'000);
        const auto audible=advance(edge,1'000'000);
        require(channel_rms(audible)>100 && channel_rms(audible)==channel_rms(audible,true),
                "one/four phrase bounds render and bounded control-only transitions restore default gain");
        (void)advance(edge,3'000'000);
        const auto ended=advance(edge,200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"one/four phrase bounds terminate silently");
    }
    for(unsigned followup:{9U,10U,11U}) {
        Host commanded(config(model,p,followup));Host::StereoSample sample;
        const unsigned token=followup==9 ? 3 : 4;
        while((commanded.icd().sound_packets_delivered()!=2 ||
               commanded.cpu().debug_wram_byte(0x20)!=(followup==9 ? 0xff : 1) ||
               commanded.cpu().debug_wram_byte(0x23)!=token) && commanded.cpu().timing().clocks()<14'000'000) {
            require(commanded.step(),"reach phrase stop/restart acknowledgment");while(commanded.pop_sample(sample)) {}
        }
        require(commanded.cpu().debug_wram_byte(0x2b)==0xc8 && commanded.cpu().debug_wram_byte(0x23)==token,
                "v8 keeps uploaded score command limits and acknowledges explicit restart/stop");
        (void)advance(commanded,200'000);
        const auto pcm=advance(commanded,800'000);
        if(followup==11) {
            require(channel_rms(pcm)>100 && channel_rms(pcm,true)>100 &&
                    std::abs(double(crossings(pcm,true))/crossings(pcm)-2)<0.25,
                    "music restart returns to first phrase and restarts both track clocks");
        } else {
            require(channel_rms(pcm)==0 && channel_rms(pcm,true)==0,"phrase stop and invalid score silence all relevant voices");
            (void)advance(commanded,5'000'000);
            const auto quiet=advance(commanded,200'000);
            require(channel_rms(quiet)==0 && channel_rms(quiet,true)==0,"stopped phrase sequence never advances later");
        }
    }
    std::vector<Payload> invalid;
    const unsigned later=p[4+0x30],last=p[4+0x42];
    auto changed=p;changed[9]=0;invalid.push_back(changed); // count zero
    changed=p;changed[9]=5;invalid.push_back(changed); // count bound
    changed=p;changed[4+0x0a]=0x20;invalid.push_back(changed); // repeated/backward phrase pointer
    changed=p;changed[4+0x0e]=1;invalid.push_back(changed); // list terminator/reserved header
    changed=p;changed[4+0x30]--;invalid.push_back(changed); // overlapping later track
    changed=p;changed[4+0x34]=1;invalid.push_back(changed); // third channel in later pattern
    changed=p;changed[4+later+1]=0xc8;invalid.push_back(changed); // no inherited held note in later phrase
    changed=p;changed[4+later]=0x98;invalid.push_back(changed); // no inherited duration
    changed=p;changed[4+last]=0xe3;invalid.push_back(changed); // unsupported control in final track
    changed=p;changed[4+p[8]-1]=0xe0;invalid.push_back(changed); // final track truncation
    for(const auto& payload:invalid) {
        Host rejected(config(model,payload));(void)advance(rejected,11'000'000);
        require(rejected.cpu().debug_wram_byte(0x24)==1 && rejected.cpu().debug_wram_byte(0x2a)==1 &&
                rejected.cpu().debug_wram_byte(0x28)>=0xe1 && rejected.cpu().debug_wram_byte(0x29)==0,
                "malformed later phrase rejects the whole bank before v8 readiness");
        const auto quiet=advance(rejected,200'000);
        require(channel_rms(quiet)==0 && channel_rms(quiet,true)==0,"rejected finite phrase bank stays silent");
    }
    Host repeated(sequence(model,{p,p,program_payload({},0x0200)}));(void)advance(repeated,20'000'000);
    require(repeated.cpu().debug_wram_byte(0x26)==3 && repeated.cpu().debug_wram_byte(0x2a)==4 &&
            repeated.cpu().debug_wram_byte(0x2b)==0xc4 && repeated.cpu().debug_wram_byte(0x20)==1,
            "repeated v8 uploads rearm and legacy restart returns to v4");
}
void resident_repeats(gameboy::HardwareModel model,const Payload& p) {
    for(bool combined:{false,true}) {
        auto c=config(model,p,5);c.combined_audio=combined;
        Host h(c),scalar(c),peer(c);scalar.debug_set_apu_batch_enabled(false);
        const auto boot=advance(h,14'000'000),reference=advance(scalar,14'000'000);
        require(equal(boot,reference) && h.save_state()==scalar.save_state(),"bounded repeats native/combined scalar parity");
        require(h.cpu().debug_wram_byte(0x2b)==0xc9 && h.cpu().debug_wram_byte(0x2a)==2,
                "repeat header and all patterns validate before v9 adoption");
        h.reset();require(equal(boot,advance(h,14'000'000)),"reset repeats upload and bounded sequence playback");
        Host measured(c);resident_ready(measured,0xc9);(void)advance(measured,100'000);
        const auto first=advance(measured,800'000);
        const auto low=crossings(first);const auto level=channel_rms(first);
        require(low>5 && level>100 && channel_rms(first,true)==0,"repeat fixture begins at authored pitch and left pan");
        (void)advance(measured,800'000);
        const auto waiting=advance(measured,500'000);
        require(channel_rms(waiting)==0 && channel_rms(waiting,true)==0,"longer first-phrase rest holds repeat barrier");
        (void)advance(measured,700'000);
        const auto high=advance(measured,800'000);
        require(std::abs(double(crossings(high))/low-2)<0.2 && std::abs(channel_rms(high)/level-0.5)<0.04,
                "last phrase in first pass changes pitch and gain");
        require(peer.load_state(measured.save_state()),"snapshot before whole-sequence wrap");
        const auto paired=[&](std::uint64_t clocks) {
            const auto pcm=advance(measured,clocks);
            require(equal(pcm,advance(peer,clocks)) && measured.save_state()==peer.save_state(),
                    "remaining play count phrase index and DSP restore exactly across wrap and termination");
            return pcm;
        };
        (void)paired(700'000);
        const auto repeated=paired(800'000);
        require(std::abs(double(crossings(repeated))/low-1)<0.2 && std::abs(channel_rms(repeated)/level-1)<0.04 &&
                channel_rms(repeated,true)==0,"second pass returns to first phrase and restores authored gain");
        (void)paired(600'000);
        const auto second_wait=paired(500'000);
        require(channel_rms(second_wait)==0 && channel_rms(second_wait,true)==0,"second pass retains longer rest barrier");
        (void)paired(800'000);
        const auto final=paired(700'000);
        require(std::abs(double(crossings(final))/low/0.875-2)<0.2 && std::abs(channel_rms(final)/level-0.5)<0.04,
                "second pass reaches final phrase without skipping tracks");
        (void)paired(1'000'000);
        const auto ended=paired(200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"two total plays terminate without infinite loop");
    }
    for(unsigned count:{1U,4U}) {
        std::vector<std::uint8_t> bank(32+16*count);
        bank[0]='G';bank[1]='B';bank[2]='S';bank[3]='5';bank[5]=count;bank[6]=4;
        for(unsigned pattern=0;pattern<count;++pattern) {
            const unsigned table=32+16*pattern;
            bank[8+2*pattern]=table;bank[9+2*pattern]=0x2b;
            for(unsigned channel=0;channel<2;++channel) {
                bank[table+2*channel]=bank.size();bank[table+2*channel+1]=0x2b;
                if(count==4) append(bank,{0xed,0,0}); // all sixteen transitions are immediate
                else append(bank,{8,static_cast<std::uint8_t>(channel==0 ? 0x8c : 0xc9),0});
            }
        }
        bank[4]=bank.size();
        Host edge(config(model,program_payload(bank,0x2b00,0x0400),5));resident_ready(edge,0xc9);
        if(count==1) {
            (void)advance(edge,8'800'000);
            require(channel_rms(advance(edge,600'000))>100,"four-play maximum reaches its fourth audible pass");
            (void)advance(edge,3'000'000);
        }
        const auto ended=advance(edge,200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"four-play maximum and sixteen immediate phases terminate");
    }
    for(unsigned followup:{9U,10U,12U}) {
        Host commanded(config(model,p,followup));Host::StereoSample sample;
        const unsigned token=followup==9 ? 3 : 4;
        while((commanded.icd().sound_packets_delivered()!=2 ||
               commanded.cpu().debug_wram_byte(0x20)!=(followup==9 ? 0xff : 1) ||
               commanded.cpu().debug_wram_byte(0x23)!=token) && commanded.cpu().timing().clocks()<18'000'000) {
            require(commanded.step(),"reach repeat stop/restart acknowledgment");while(commanded.pop_sample(sample)) {}
        }
        require(commanded.cpu().debug_wram_byte(0x2b)==0xc9 && commanded.cpu().debug_wram_byte(0x23)==token,
                "v9 keeps score limits and acknowledges repeat stop/restart");
        (void)advance(commanded,200'000);
        if(followup==12) {
            const auto restarted=advance(commanded,800'000);
            require(crossings(restarted)>5 && channel_rms(restarted)>100 && channel_rms(restarted,true)==0,
                    "restart returns to first phrase with fresh track clocks");
            (void)advance(commanded,3'400'000);
            require(channel_rms(advance(commanded,600'000))>100,"restart replenishes remaining plays for another pass");
            (void)advance(commanded,4'000'000);
        } else {
            const auto quiet=advance(commanded,500'000);
            require(channel_rms(quiet)==0 && channel_rms(quiet,true)==0,"repeat stop and invalid score silence music/effects");
            (void)advance(commanded,10'000'000);
        }
        const auto ended=advance(commanded,200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"stopped or restarted bounded sequence remains finite");
    }
    std::vector<Payload> invalid;
    for(unsigned plays:{0U,5U,128U}) {auto changed=p;changed[10]=plays;invalid.push_back(changed);}
    auto changed=p;changed[7]='4';invalid.push_back(changed); // GBS4 reserves count byte
    changed=p;changed[11]=1;invalid.push_back(changed); // reserved header
    const unsigned later=p[4+0x30];
    changed=p;changed[4+later+7]=0xc8;invalid.push_back(changed); // later phrase initial tie
    changed=p;changed[4+p[8]-1]=0xe0;invalid.push_back(changed); // final stream truncation
    for(const auto& payload:invalid) {
        Host rejected(config(model,payload));(void)advance(rejected,11'000'000);
        require(rejected.cpu().debug_wram_byte(0x24)==1 && rejected.cpu().debug_wram_byte(0x2a)==1 &&
                rejected.cpu().debug_wram_byte(0x28)>=0xe1 && rejected.cpu().debug_wram_byte(0x29)==0,
                "bad repeat count or later stream rejects entire bank before v9 readiness");
    }
    Host repeated(sequence(model,{p,p,program_payload({},0x0200)}));(void)advance(repeated,20'000'000);
    require(repeated.cpu().debug_wram_byte(0x26)==3 && repeated.cpu().debug_wram_byte(0x2a)==4 &&
            repeated.cpu().debug_wram_byte(0x2b)==0xc4 && repeated.cpu().debug_wram_byte(0x20)==1,
            "repeated v9 uploads rearm and legacy restart returns to v4");
}
void resident_transpose(gameboy::HardwareModel model,const Payload& p) {
    for(bool combined:{false,true}) {
        auto c=config(model,p,5);c.combined_audio=combined;
        Host h(c),scalar(c),peer(c);scalar.debug_set_apu_batch_enabled(false);
        const auto boot=advance(h,16'000'000),reference=advance(scalar,16'000'000);
        require(equal(boot,reference) && h.save_state()==scalar.save_state(),"transpose native/combined scalar parity");
        require(h.cpu().debug_wram_byte(0x2b)==0xca && h.cpu().debug_wram_byte(0x2a)==2,
                "all effective pitches validate before v10 adoption");
        h.reset();require(equal(boot,advance(h,16'000'000)),"reset repeats transposed playback and phrase repeats");
        Host measured(c);resident_ready(measured,0xca);(void)advance(measured,100'000);
        const auto first=advance(measured,1'000'000);
        const auto low=crossings(first),right=crossings(first,true);
        require(low>5 && std::abs(double(right)/low-1)<0.15,"negative transpose makes right base octave match left note");
        require(peer.load_state(measured.save_state()),"snapshot before transpose command and held tie");
        const auto paired=[&](std::uint64_t clocks) {
            const auto pcm=advance(measured,clocks);
            require(equal(pcm,advance(peer,clocks)) && measured.save_state()==peer.save_state(),
                    "per-track transpose and held DSP pitch restore through controls phrases and wrap");
            return pcm;
        };
        (void)paired(600'000);
        const auto tie=paired(800'000);
        require(std::abs(double(crossings(tie))/low/0.8-1)<0.15,"transpose command on held tie keeps previous pitch");
        (void)paired(400'000);
        const auto octave=paired(800'000);
        require(std::abs(double(crossings(octave))/low/0.8-2)<0.2 &&
                std::abs(double(crossings(octave,true))/right/0.8-1)<0.15,
                "positive transpose affects next left note while right track remains independent");
        (void)paired(700'000);
        const auto down=paired(800'000);
        require(std::abs(double(crossings(down))/low/0.8-0.5)<0.15 &&
                std::abs(double(crossings(down,true))/right/0.8-1)<0.15,
                "negative transpose lowers next left note without changing right pitch");
        (void)paired(500'000);
        const auto fresh=paired(800'000);
        require(std::abs(double(crossings(fresh))/low/0.8-1)<0.15 &&
                std::abs(double(crossings(fresh,true))/right/0.8-2)<0.2,
                "phrase boundary resets transpose independently on both tracks");
        (void)paired(600'000);
        const auto wrapped=paired(800'000);
        require(std::abs(double(crossings(wrapped))/low/0.8-1)<0.15 &&
                std::abs(double(crossings(wrapped,true))/right/0.8-1)<0.15,
                "whole-sequence wrap clears trailing transpose before the first note");
        (void)paired(6'000'000);
        const auto ended=paired(200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"transposed repeated score remains finite");
    }
    std::vector<std::uint8_t> edge_bank(48);
    edge_bank[0]='G';edge_bank[1]='B';edge_bank[2]='S';edge_bank[3]='6';edge_bank[5]=1;edge_bank[6]=1;
    edge_bank[8]=0x20;edge_bank[9]=0x2b;
    for(unsigned channel=0;channel<2;++channel) {
        edge_bank[32+2*channel]=edge_bank.size();edge_bank[33+2*channel]=0x2b;
        append(edge_bank,{0xe0,static_cast<std::uint8_t>(channel),0xe1,static_cast<std::uint8_t>(20*channel),0xed,64,
            0xea,static_cast<std::uint8_t>(channel==0 ? 0xf4 : 12),16,static_cast<std::uint8_t>(channel==0 ? 0x8c : 0x93),0});
    }
    edge_bank[4]=edge_bank.size();
    Host edge(config(model,program_payload(edge_bank,0x2b00,0x0400),5));resident_ready(edge,0xca);
    (void)advance(edge,100'000);const auto limits=advance(edge,2'000'000);
    require(crossings(limits)>5 && std::abs(double(crossings(limits,true))/crossings(limits)-6)<0.35,
            "transpose boundaries render pitch indices zero and thirty-one without wrap or clamp");
    (void)advance(edge,4'000'000);const auto edge_ended=advance(edge,200'000);
    require(channel_rms(edge_ended)==0 && channel_rms(edge_ended,true)==0,"boundary pitches terminate silently");
    std::vector<Payload> invalid;
    const unsigned start=p[4+0x20],other=p[4+0x22];
    auto changed=p;changed[7]='5';invalid.push_back(changed); // older envelope rejects EA
    changed=p;changed[4+start+9]=13;invalid.push_back(changed); // positive limit
    changed=p;changed[4+start+15]=0xf3;invalid.push_back(changed); // negative limit
    changed=p;changed[4+start+9]=0x80;invalid.push_back(changed); // signed -128
    changed=p;changed[4+start+13]=0x9f;invalid.push_back(changed); // effective note above 31
    changed=p;changed[4+other+9]=0x80;invalid.push_back(changed); // effective note below zero
    changed=p;changed[4+p[8]-1]=0xea;invalid.push_back(changed); // missing operand/terminator
    for(const auto& payload:invalid) {
        Host rejected(config(model,payload));(void)advance(rejected,11'000'000);
        require(rejected.cpu().debug_wram_byte(0x24)==1 && rejected.cpu().debug_wram_byte(0x2a)==1 &&
                rejected.cpu().debug_wram_byte(0x28)==0xe2 && rejected.cpu().debug_wram_byte(0x29)==0,
                "bad transpose or effective pitch rejects the whole bank before readiness");
    }
    for(unsigned followup:{9U,10U,12U}) {
        Host commanded(config(model,p,followup));Host::StereoSample sample;
        const unsigned token=followup==9 ? 3 : 4;
        while((commanded.icd().sound_packets_delivered()!=2 ||
               commanded.cpu().debug_wram_byte(0x20)!=(followup==9 ? 0xff : 1) ||
               commanded.cpu().debug_wram_byte(0x23)!=token) && commanded.cpu().timing().clocks()<18'000'000) {
            require(commanded.step(),"reach transpose stop/restart acknowledgment");while(commanded.pop_sample(sample)) {}
        }
        require(commanded.cpu().debug_wram_byte(0x2b)==0xca && commanded.cpu().debug_wram_byte(0x23)==token,
                "v10 keeps score limits and acknowledges transpose stop/restart");
        (void)advance(commanded,200'000);
        if(followup==12) {
            const auto restarted=advance(commanded,800'000);
            require(crossings(restarted)>5 && std::abs(double(crossings(restarted,true))/crossings(restarted)-1)<0.2,
                    "music restart clears both transposes and returns to authored initial pitches");
            (void)advance(commanded,14'000'000);
        } else {
            const auto quiet=advance(commanded,500'000);
            require(channel_rms(quiet)==0 && channel_rms(quiet,true)==0,"transpose stop and invalid score silence music/effects");
            (void)advance(commanded,14'000'000);
        }
        const auto ended=advance(commanded,200'000);
        require(channel_rms(ended)==0 && channel_rms(ended,true)==0,"stopped or restarted transpose score remains finite");
    }
    Host repeated(sequence(model,{p,p,program_payload({},0x0200)}));(void)advance(repeated,20'000'000);
    require(repeated.cpu().debug_wram_byte(0x26)==3 && repeated.cpu().debug_wram_byte(0x2a)==4 &&
            repeated.cpu().debug_wram_byte(0x2b)==0xc4 && repeated.cpu().debug_wram_byte(0x20)==1,
            "repeated v10 uploads rearm and legacy restart returns to v4");
}
void uploaded_scores(gameboy::HardwareModel model,const Payload& p) {
    auto parsed=gameboy::SgbSoundTransfer::parse(p);
    require(parsed.valid() && parsed.writes.size()==1 && parsed.writes[0].destination==0x07d0 &&
            parsed.writes[0].size==16 && parsed.jump_address==0x0200,
            "packer output is exactly one score-table block and a resident restart");
    for(unsigned score=0;score<2;++score) for(bool combined:{false,true}) {
        auto c=config(model,p,5+score);c.combined_audio=combined;
        Host h(c),scalar(c),peer(c);scalar.debug_set_apu_batch_enabled(false);
        const auto boot=advance(h,8'000'000,true);
        require(h.cpu().debug_wram_byte(0x20)==1 && h.cpu().debug_wram_byte(0x24)==0 &&
                h.cpu().debug_wram_byte(0x26)==1 && h.cpu().debug_wram_byte(0x2a)==2 &&
                h.cpu().debug_wram_byte(0x2b)==0xc4,"data-only transfer restarts and adopts original v4 driver");
        latched(h,p);
        require(equal(boot,advance(scalar,8'000'000)) && h.save_state()==scalar.save_state(),
                "uploaded scores match scalar native/combined audio and state");
        require(peer.load_state(h.save_state()),"restore uploaded score table and active note countdown");
        require(equal(advance(h,1'000'000),advance(peer,1'000'000)) && h.save_state()==peer.save_state(),
                "uploaded score state continues identically across instances");
        h.reset();require(equal(boot,advance(h,8'000'000)),"reset reproduces score capture, data upload and music");
        // Measure the first note in isolation, against an independently packed
        // transfer of the built-in motifs. This detects a driver ignoring the
        // uploaded data even if its built-in music remains audible.
        Host measured(c);ready_score(measured);(void)advance(measured,100'000);
        const auto actual=crossings(advance(measured,1'000'000));
        auto defaults=p;
        const std::array<std::uint8_t,16> pitches{4,5,6,8,6,5,4,3,8,6,4,6,9,6,4,3};
        std::copy(pitches.begin(),pitches.end(),defaults.begin()+4);
        auto reference_config=config(model,defaults,5+score);reference_config.combined_audio=combined;
        Host reference(reference_config);ready_score(reference);(void)advance(reference,100'000);
        const auto expected=crossings(advance(reference,1'000'000));
        const double ratio=double(p[4+score*8])/pitches[score*8];
        require(actual>0 && expected>0 && std::abs(double(actual)/expected-ratio)<ratio*0.08,
                "rendered first note follows the uploaded pitch instead of the bundled score");
    }
}
void incompatible(gameboy::HardwareModel model) {
    // Original test stubs explicitly advertise wrong versions or violate one
    // advertised operation. They execute normally; the host must fail boundedly.
    std::vector<std::uint8_t> wrong_version{0x8f,0xcb,0xf5,0x8f,0xa5,0xf7,0x8f,0x5a,0xf4,
        0xe4,0xf5,0xc4,0xf6,0x2f,0xfa}; // observe input 1; host must never write it
    Host unknown(config(model,program_payload(wrong_version,0x0800),1));
    (void)advance(unknown,10'000'000);
    require(unknown.cpu().debug_wram_byte(0x20)==0xff && unknown.cpu().debug_wram_byte(0x24)==1 &&
            unknown.cpu().debug_wram_byte(0x2a)==1,"unknown mailbox version remains external");
    require(unknown.cpu().debug_wram_byte(0x28)==0,"unknown driver receives no SOUND parameters");
    std::vector<std::uint8_t> no_arm{0x8f,0xc1,0xf5,0x8f,0xa5,0xf7,0x8f,0x5a,0xf4,0x2f,0xfe};
    Host arm(config(model,program_payload(no_arm,0x0800)));(void)advance(arm,16'000'000);
    require(arm.cpu().debug_wram_byte(0x20)==0xff && arm.cpu().debug_wram_byte(0x27)==5 &&
            arm.cpu().debug_wram_byte(0x2a)==1,"missing arm acknowledgment times out before adoption");
    auto no_sound=no_arm;no_sound.resize(9);
    append(no_sound,{0xe4,0xf4,0x68,0,0xd0,0xfa,0x8f,0,0xf4,0x2f,0xfe});
    Host sound(config(model,program_payload(no_sound,0x0800),1));(void)advance(sound,16'000'000);
    require(sound.cpu().debug_wram_byte(0x20)==0xff && sound.cpu().debug_wram_byte(0x27)==5 &&
            sound.cpu().debug_wram_byte(0x2a)==2,"compatible advertisement cannot cause unbounded SOUND polling");
    Host stop(config(model,program_payload(no_sound,0x0800),3));(void)advance(stop,16'000'000);
    require(stop.cpu().debug_wram_byte(0x20)==0xff && stop.cpu().debug_wram_byte(0x27)==5 &&
            stop.cpu().debug_wram_byte(0x24)==1,"failed stop acknowledgment halts without retry recursion");
    auto no_loader=no_sound;no_loader.resize(no_loader.size()-2);
    append(no_loader,{0xe4,0xf4,0xc4,0xf4,0x2f,0xfa}); // echo tokens but never return to IPL
    Host legacy(config(model,program_payload(no_loader,0x0800),1));(void)advance(legacy,10'000'000);
    require(legacy.cpu().debug_wram_byte(0x20)==1 && legacy.cpu().debug_wram_byte(0x2b)==0xc1 &&
            legacy.cpu().debug_wram_byte(0x23)==1,"legacy v1 SOUND dispatch uses one token without attribute staging");
    Host legacy_attributes(config(model,program_payload(no_loader,0x0800),3));(void)advance(legacy_attributes,10'000'000);
    require(legacy_attributes.cpu().debug_wram_byte(0x20)==0xff && legacy_attributes.cpu().debug_wram_byte(0x24)==0 &&
            legacy_attributes.cpu().debug_wram_byte(0x23)==1,"legacy v1 rejects new attributes and acknowledges ordinary stop");
    auto v2=no_loader;v2[1]=0xc2;
    Host attributes(config(model,program_payload(v2,0x0800),3));(void)advance(attributes,10'000'000);
    require(attributes.cpu().debug_wram_byte(0x20)==1 && attributes.cpu().debug_wram_byte(0x2b)==0xc2 && attributes.cpu().debug_wram_byte(0x23)==2,"v2 adoption retains two-phase attribute commands");
    for(unsigned followup:{4U,5U}) {
        Host limited(config(model,program_payload(v2,0x0800),followup));(void)advance(limited,10'000'000);
        require(limited.cpu().debug_wram_byte(0x20)==0xff && limited.cpu().debug_wram_byte(0x23)==1,"v2 rejects v3 presets/scores before staging and acknowledges legacy stop");
    }
    auto v3=no_loader;v3[1]=0xc3;
    Host old_bank(config(model,program_payload(v3,0x0800),8));(void)advance(old_bank,10'000'000);
    require(old_bank.cpu().debug_wram_byte(0x20)==1 && old_bank.cpu().debug_wram_byte(0x2b)==0xc3 && old_bank.cpu().debug_wram_byte(0x23)==2,"v3 retains original bank and score command staging");
    Host old_limit(config(model,program_payload(v3,0x0800),7));(void)advance(old_limit,10'000'000);
    require(old_limit.cpu().debug_wram_byte(0x20)==0xff && old_limit.cpu().debug_wram_byte(0x23)==1,"v3 rejects v4 presets before staging and acknowledges silence-all");
    Host loader(config(model,program_payload(no_loader,0x0800),2));(void)advance(loader,14'000'000);
    require(loader.cpu().debug_wram_byte(0x20)==0xff && loader.cpu().debug_wram_byte(0x27)==5 &&
            loader.cpu().debug_wram_byte(0x25)==2 && loader.cpu().debug_wram_byte(0x26)==1,
            "missing cooperative IPL return times out without a second upload");
}

void invalid(gameboy::HardwareModel model) {
    std::vector<Payload> cases;
    Payload p{}; // zero jump: transport-reserved address
    cases.push_back(p);
    p={};p[0]=0xff;p[1]=0xff;p[3]=2;cases.push_back(p); // source arithmetic wrap
    p={};p[0]=0xfd;p[1]=0x0f;p[3]=2;cases.push_back(p); // source exceeds screen by one
    p={};p[0]=4;p[2]=0xfe;p[3]=0xff;cases.push_back(p); // destination wrap
    p={};p[0]=1;p[2]=0xf4;cases.push_back(p); // I/O overwrite
    p={};p[3]=0xff;p[2]=0xc0;cases.push_back(p); // jump into IPL overlay
    p={};p[0]=0xfc;p[1]=0x0f;p[3]=2;cases.push_back(p); // exactly 4096 consumed, no jump
    p={};p[0]=0xfb;p[1]=0x0f;p[3]=2;cases.push_back(p); // one-byte final header
    for(const auto& payload:cases) {
        Host h(config(model,payload));(void)advance(h,9'000'000,true);
        latched(h,payload);
        require(h.cpu().debug_wram_byte(0x20)==0xff && h.cpu().debug_wram_byte(0x27)!=0,"invalid list rejected");
        require(h.cpu().debug_wram_byte(0x26)==0 && h.cpu().debug_wram_byte(0x24)==0,"invalid list never releases SPC ownership or uploads");
    }
    std::uint8_t sum=0,x=0;Host lcd_off(config(model,valid(sum,x),false,false));
    (void)advance(lcd_off,9'000'000);
    require(lcd_off.cpu().debug_wram_byte(0x20)==0xff && lcd_off.cpu().debug_wram_byte(0x27)==4,"LCD-off transfer fails within bounded wait");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && (std::string(argv[1])=="--score-transfer" || std::string(argv[1])=="--resident-score" || std::string(argv[1])=="--resident-controls" || std::string(argv[1])=="--resident-tracks" || std::string(argv[1])=="--resident-phrases" || std::string(argv[1])=="--resident-repeats" || std::string(argv[1])=="--resident-transpose")) {
            Payload p{};std::ifstream input(argv[2],std::ios::binary);
            require(bool(input.read(reinterpret_cast<char*>(p.data()),p.size())) && input.peek()==std::char_traits<char>::eof(),"score transfer file must contain exactly 4096 bytes");
            for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
                if(std::string(argv[1])=="--resident-transpose") resident_transpose(model,p);
                else if(std::string(argv[1])=="--resident-repeats") resident_repeats(model,p);
                else if(std::string(argv[1])=="--resident-phrases") resident_phrases(model,p);
                else if(std::string(argv[1])=="--resident-tracks") resident_tracks(model,p);
                else if(std::string(argv[1])=="--resident-controls") resident_controls(model,p);
                else if(std::string(argv[1])=="--resident-score") resident_score(model,p);
                else uploaded_scores(model,p);
            }
            std::cout<<"Original SGB1/SGB2 uploaded scores: data-only handoff, note pitches, native/combined scalar PCM, reset and restore passed\n";
            return 0;
        }
        require(argc==1,"expected --score-transfer FILE or no arguments");
        for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) { upload(model);invalid(model);interoperability(model);incompatible(model);unsupported_score_restart(model); }
        std::cout<<"Original SGB1/SGB2: exact 4K ICD capture, validated multi-block SOU_TRN, upper-RAM upload, SPC handoff/audio, versioned driver adoption, repeated transfers, SOUND, timeouts, reset and restore passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
