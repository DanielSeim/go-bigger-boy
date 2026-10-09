#include "gameboy/snes_apu_audio_engine.hpp"
#include "../firmware/spc700/ipl_image.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
class Upload {
public:
    gameboy::SnesApuAudioEngine engine;
    bool restore{};
    unsigned restores{};
    std::vector<std::uint64_t> timeline;
    explicit Upload(const gameboy::SnesApuBus::IplRom& image, bool roundtrip) : restore(roundtrip) {
        engine.install_ipl(image);
    }
    static bool clock(gameboy::SnesApuAudioEngine& apu) {
        gameboy::SnesApuAudioEngine::StereoSample sample;
        while(apu.pop_sample(sample)) {}
        return apu.clock_half();
    }
    void checkpoint() {
        if (!restore) return;
        const auto saved = engine.save_state();
        gameboy::SnesApuAudioEngine other;
        require(other.load_state(saved), "cross-instance APU restore");
        for (unsigned i=0; i<37; ++i) {
            require(clock(engine) && clock(other), "restored continuation runs");
        }
        require(engine.save_state()==other.save_state(), "restored physical halves agree");
        require(engine.load_state(saved), "rewind to uploaded-byte boundary");
        require(engine.save_state()==saved, "exact snapshot roundtrip");
        ++restores;
    }
    template<class Predicate> void until(Predicate done) {
        for (unsigned i=0; i<40000; ++i) {
            if (done()) return;
            if (!clock(engine)) throw std::runtime_error("IPL fault at PC " + std::to_string(engine.cpu().registers().pc));
            if (i==1 || i==11) checkpoint(); // includes pending instructions/read halves
        }
        const auto& r=engine.cpu().registers();
        throw std::runtime_error("IPL protocol timed out: pc="+std::to_string(r.pc)+
            " y="+std::to_string(r.y)+" echo="+std::to_string(engine.bus().host_read_port(0))+
            " incoming="+std::to_string(engine.bus().spc_read(0xf4)));
    }
    void ready() {
        until([&]{return engine.bus().host_read_port(0)==0xaa && engine.bus().host_read_port(1)==0xbb;});
        require(engine.cpu().half_cycles()==4808,"ready signature timing contract");
        require(engine.cpu().registers().sp==0xef, "IPL stack initialization");
        for (unsigned a=1; a<0xf0; ++a)
            require(engine.bus().dsp_read_ram(a)==0, "IPL clears low RAM");
        timeline.push_back(engine.cpu().half_cycles());
    }
    void command(unsigned destination, unsigned mode, unsigned token) {
        auto& bus=engine.bus();
        bus.host_write_port(2,destination);
        bus.host_write_port(3,destination>>8);
        bus.host_write_port(1,mode);
        bus.host_write_port(0,token);
        until([&]{return bus.host_read_port(0)==token;});
        timeline.push_back(engine.cpu().half_cycles());
    }
    void block(unsigned destination, unsigned size, unsigned token) {
        command(destination, 1, token);
        for (unsigned i=0; i<size; ++i) {
            // Exercise waiting polls as well as an immediately responding host.
            if(i%23==0) for(unsigned delay=0;delay<55+(i%7)*13;++delay)
                require(clock(engine),"delayed host polling remains supported");
            const auto value=static_cast<std::uint8_t>((i*73 + i/7 + 19)&255);
            engine.bus().host_write_port(1,value);
            engine.bus().host_write_port(0,i&255);
            try { until([&]{return engine.bus().host_read_port(0)==(i&255);}); }
            catch(const std::exception& e) { throw std::runtime_error("byte "+std::to_string(i)+": "+e.what()); }
            timeline.push_back(engine.cpu().half_cycles());
            // Acknowledgment can precede the RAM store in the reference.
            until([&]{return engine.bus().dsp_read_ram(destination+i)==value;});
            timeline.push_back(engine.cpu().half_cycles());
            if (i==0 || i==255 || i==256 || i==size-1) checkpoint();
        }
        for (unsigned i=0; i<size; ++i)
            require(engine.bus().dsp_read_ram(destination+i)==((i*73+i/7+19)&255), "all uploaded bytes match");
    }
    void execute(unsigned destination, unsigned token) {
        // Original payload writes DSP KON and spins, independently of IPL.
        const std::uint8_t program[]{0x8f,0x4c,0xf2,0x8f,1,0xf3,0x2f,0xfe};
        for (unsigned i=0;i<sizeof(program);++i) engine.bus().spc_write(destination+i,program[i]);
        command(destination,0,token);
        until([&]{return !engine.cpu().instruction_pending() && engine.cpu().registers().pc==destination;});
        const auto& r=engine.cpu().registers();
        require(r.a==0 && r.x==0 && r.y==0 && r.sp==0xef, "entry register contract");
        checkpoint();
        until([&]{return engine.bus().dsp_register(0x4c)==1;});
        timeline.push_back(engine.cpu().half_cycles());
    }
};
std::vector<std::uint64_t> exercise(const gameboy::SnesApuBus::IplRom& image, bool restore) {
    Upload test(image,restore);
    for (unsigned a=1;a<0xf0;++a) test.engine.bus().spc_write(a,0x7b);
    test.ready();
    // Unaligned destination, two counter wraps, then another block.
    test.block(0x02f3,600,0xcc);
    test.block(0x1807,257,((600+2)|1)&255);
    test.block(0x7f83,600,((257+2)|1)&255);
    test.execute(0x2100,((600+2)|1)&255);
    if (restore) require(test.restores>20, "mid-upload restoration coverage");
    test.engine.reset();
    test.ready();
    // Direct entry with no transfer; reset must retain the configured image.
    test.execute(0x2200,0xcc);
    std::cout << "upload, wraps, multiple blocks, handoff, reset; " << test.restores << " restores\n";
    return test.timeline;
}
}
int main(int argc,char** argv) {
    try {
        const auto image=gameboy::firmware::spc700_ipl_image;
        const auto expected=exercise(image,false);
        require(exercise(image,true)==expected,"restoration preserves entire upload timeline");
        if(argc==2) {
            gameboy::SnesApuBus::IplRom reference{};
            std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
            require(file && file.tellg()==64,"reference IPL must be exactly 64 bytes");
            file.seekg(0);
            require(bool(file.read(reinterpret_cast<char*>(reference.data()),64)),"read opaque reference IPL");
            require(exercise(reference,false)==expected,"opaque reference upload/handoff clocks match");
            require(exercise(reference,true)==expected,"opaque reference restored clocks match");
        } else require(argc==1,"usage: spc700_replacement_ipl_tests [opaque-reference-image]");
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
