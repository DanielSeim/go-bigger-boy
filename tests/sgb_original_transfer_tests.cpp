#include "gameboy/sgb_host.hpp"
#include "gameboy/sgb_sound.hpp"
#include "../firmware/sgb/prototype_image.hpp"
#include <iostream>
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
            followup==3 ? Packet{0x41,1,0,1} : Packet{0x49});
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
        while(!predicate() && a.cpu().timing().clocks()<4'000'000) (void)advance(a,500);
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
    const auto pcm=advance(h,4'000'000,true),reference=advance(scalar,4'000'000);
    latched(h,p);
    require(h.cpu().debug_wram_byte(0x20)==2,"uploaded driver owns APU");
    require(h.cpu().debug_wram_byte(0x26)==1 && h.cpu().debug_wram_byte(0x27)==0,"one valid upload and jump");
    require(h.cpu().debug_wram_byte(0x28)==sum && h.cpu().debug_wram_byte(0x29)==xor_sum,
            "uploaded SPC program verifies all transferred data including upper-RAM boundary");
    require(std::any_of(pcm.begin(),pcm.end(),[](auto s){return s.left>100 || s.left < -100;}),"uploaded code renders DSP audio");
    require(equal(pcm,reference) && h.save_state()==scalar.save_state(),"scalar upload and playback parity");
    require(restored.load_state(h.save_state()),"cross-instance post-handoff restore");
    require(equal(advance(h,200'000),advance(restored,200'000)) && h.save_state()==restored.save_state(),"post-handoff continuation");
    h.reset();require(equal(pcm,advance(h,4'000'000)),"reset repeats physical capture/upload/audio");
    Host next_frame(config(model,p,0,true,true));(void)advance(next_frame,4'000'000);
    auto changed=p;
    for(unsigned i=0;i<16;++i) changed[0xff0+i]=0xa0+i;
    latched(next_frame,changed);
    require(next_frame.cpu().debug_wram_byte(0x26)==1,"next-frame update remains a valid upload");
    // Stop assumptions at the ownership boundary: unsupported SOUND must not
    // send our mailbox token or stop codes to an unrelated uploaded program.
    for(unsigned command:{1U,2U}) {
        Host external(config(model,p,command));(void)advance(external,6'000'000);
        require(external.cpu().debug_wram_byte(0x20)==0xff &&
                external.cpu().debug_wram_byte(0x21)==(command==1 ? 0x41 : 0x49),
                "external SOUND/SOU_TRN rejected explicitly");
        require(external.cpu().debug_wram_byte(0x28)==sum && external.cpu().debug_wram_byte(0x29)==xor_sum,"external output ports remain untouched");
    }
}
Payload program_payload(const std::vector<std::uint8_t>& program,unsigned destination) {
    Payload p{};unsigned offset=0;
    const auto word=[&](unsigned v){p[offset++]=v;p[offset++]=v>>8;};
    require(program.size()+8<=p.size(),"uploaded test driver fits transfer screen");
    if(!program.empty()) {
        word(program.size());word(destination);
        for(auto b:program) p[offset++]=b;
    }
    word(0);word(destination);return p;
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
    std::vector<std::uint8_t> driver(gameboy::firmware::sgb_prototype_spc.begin(),
                                     gameboy::firmware::sgb_prototype_spc.begin()+766);
    const std::vector<Payload> payloads{program_payload(driver,0x0800),
        program_payload(driver,0x0c00),program_payload({},0x0200)};
    auto c=sequence(model,payloads);Host h(c),scalar(c),peer(c);
    scalar.debug_set_apu_batch_enabled(false);
    const auto pcm=advance(h,12'000'000),expected=advance(scalar,12'000'000);
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
    h.reset();require(equal(pcm,advance(h,12'000'000)),"reset repeats multiple driver adoptions and SOUND");
    const auto quiet=advance(h,500'000);
    require(std::all_of(quiet.begin(),quiet.end(),[](auto s){return s.left==0 && s.right==0;}),"stop remains silent after reacquisition");
    Host negotiating(c),restored(c);Host::StereoSample sample;
    while(!(negotiating.cpu().debug_wram_byte(0x20)==5 && negotiating.cpu().debug_wram_byte(0x26)==1)
          && negotiating.cpu().timing().clocks()<4'000'000) {
        require(negotiating.step(),"reach uploaded-driver arm gate");
        while(negotiating.pop_sample(sample)) {}
    }
    require(negotiating.cpu().debug_wram_byte(0x20)==5,"snapshot while driver is being armed");
    require(restored.load_state(negotiating.save_state()),"restore in-flight arm handshake");
    require(equal(advance(negotiating,20'000),advance(restored,20'000)) &&
            negotiating.save_state()==restored.save_state(),"in-flight driver adoption continues identically");
    if(model==gameboy::HardwareModel::sgb2) {
        Host wrapped(sequence(model,{payloads.front()},true));(void)advance(wrapped,13'000'000);
        require(wrapped.cpu().debug_wram_byte(0x20)==1 && wrapped.cpu().debug_wram_byte(0x27)==0,
                "ordinary mailbox token zero is valid after counter rollover");
        require(wrapped.icd().sound_packets_delivered()==258 && wrapped.cpu().debug_wram_byte(0x23)==4,
                "256 further SOUND packets cross v2 token rollover exactly twice");
    }
}
void incompatible(gameboy::HardwareModel model) {
    // Original test stubs explicitly advertise wrong versions or violate one
    // advertised operation. They execute normally; the host must fail boundedly.
    std::vector<std::uint8_t> wrong_version{0x8f,0xc3,0xf5,0x8f,0xa5,0xf7,0x8f,0x5a,0xf4,
        0xe4,0xf5,0xc4,0xf6,0x2f,0xfa}; // observe input 1; host must never write it
    Host unknown(config(model,program_payload(wrong_version,0x0800),1));
    (void)advance(unknown,6'000'000);
    require(unknown.cpu().debug_wram_byte(0x20)==0xff && unknown.cpu().debug_wram_byte(0x24)==1 &&
            unknown.cpu().debug_wram_byte(0x2a)==1,"unknown mailbox version remains external");
    require(unknown.cpu().debug_wram_byte(0x28)==0,"unknown driver receives no SOUND parameters");
    std::vector<std::uint8_t> no_arm{0x8f,0xc1,0xf5,0x8f,0xa5,0xf7,0x8f,0x5a,0xf4,0x2f,0xfe};
    Host arm(config(model,program_payload(no_arm,0x0800)));(void)advance(arm,8'000'000);
    require(arm.cpu().debug_wram_byte(0x20)==0xff && arm.cpu().debug_wram_byte(0x27)==5 &&
            arm.cpu().debug_wram_byte(0x2a)==1,"missing arm acknowledgment times out before adoption");
    auto no_sound=no_arm;no_sound.resize(9);
    append(no_sound,{0xe4,0xf4,0x68,0,0xd0,0xfa,0x8f,0,0xf4,0x2f,0xfe});
    Host sound(config(model,program_payload(no_sound,0x0800),1));(void)advance(sound,12'000'000);
    require(sound.cpu().debug_wram_byte(0x20)==0xff && sound.cpu().debug_wram_byte(0x27)==5 &&
            sound.cpu().debug_wram_byte(0x2a)==2,"compatible advertisement cannot cause unbounded SOUND polling");
    Host stop(config(model,program_payload(no_sound,0x0800),3));(void)advance(stop,12'000'000);
    require(stop.cpu().debug_wram_byte(0x20)==0xff && stop.cpu().debug_wram_byte(0x27)==5 &&
            stop.cpu().debug_wram_byte(0x24)==1,"failed stop acknowledgment halts without retry recursion");
    auto no_loader=no_sound;no_loader.resize(no_loader.size()-2);
    append(no_loader,{0xe4,0xf4,0xc4,0xf4,0x2f,0xfa}); // echo tokens but never return to IPL
    Host legacy(config(model,program_payload(no_loader,0x0800),1));(void)advance(legacy,6'000'000);
    require(legacy.cpu().debug_wram_byte(0x20)==1 && legacy.cpu().debug_wram_byte(0x2b)==0xc1 &&
            legacy.cpu().debug_wram_byte(0x23)==1,"legacy v1 SOUND dispatch uses one token without attribute staging");
    Host legacy_attributes(config(model,program_payload(no_loader,0x0800),3));(void)advance(legacy_attributes,6'000'000);
    require(legacy_attributes.cpu().debug_wram_byte(0x20)==0xff && legacy_attributes.cpu().debug_wram_byte(0x24)==0 &&
            legacy_attributes.cpu().debug_wram_byte(0x23)==1,"legacy v1 rejects new attributes and acknowledges ordinary stop");
    Host loader(config(model,program_payload(no_loader,0x0800),2));(void)advance(loader,10'000'000);
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
        Host h(config(model,payload));(void)advance(h,3'000'000,true);
        latched(h,payload);
        require(h.cpu().debug_wram_byte(0x20)==0xff && h.cpu().debug_wram_byte(0x27)!=0,"invalid list rejected");
        require(h.cpu().debug_wram_byte(0x26)==0 && h.cpu().debug_wram_byte(0x24)==0,"invalid list never releases SPC ownership or uploads");
    }
    std::uint8_t sum=0,x=0;Host lcd_off(config(model,valid(sum,x),false,false));
    (void)advance(lcd_off,7'000'000);
    require(lcd_off.cpu().debug_wram_byte(0x20)==0xff && lcd_off.cpu().debug_wram_byte(0x27)==4,"LCD-off transfer fails within bounded wait");
}
}
int main() {
    try {
        for(auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) { upload(model);invalid(model);interoperability(model);incompatible(model); }
        std::cout<<"Original SGB1/SGB2: exact 4K ICD capture, validated multi-block SOU_TRN, upper-RAM upload, SPC handoff/audio, versioned driver adoption, repeated transfers, SOUND, timeouts, reset and restore passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
