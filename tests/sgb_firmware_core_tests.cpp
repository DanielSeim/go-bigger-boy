#include "gbb/core_registry.hpp"
#include "gbb/core_runtime.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/core_contract.hpp"
#include "gameboy/sgb_host.hpp"
#include "desktop_launch_options.hpp"
#include "desktop_firmware_settings.hpp"
#include "sgb_input_script.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void check(bool value,const char* what) { if(!value) throw std::runtime_error(what); }
template<class F> void rejects(F fn,const char* what) {
    bool rejected=false; try { fn(); } catch(const std::exception&) { rejected=true; }
    check(rejected,what);
}
template<class Bytes> void write(const std::filesystem::path& path,const Bytes& bytes) {
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    check(bool(out),"fixture file written");
}
gameboy::SgbHostConfig fixture() {
    gameboy::SgbHostConfig c; c.combined_audio=true;
    c.program_rom.resize(0x40000); c.program_rom[0x7fd5]=0x20;
    c.program_rom[0x7ffc]=4; c.program_rom[0x7ffd]=0x81;
    const std::uint8_t snes[]{0x78,0xa9,0x81,0x8d,3,0x60,0xe6,0,0x80,0xfc};
    std::copy(std::begin(snes),std::end(snes),c.program_rom.begin()+0x104);
    c.game_rom.resize(32768); c.game_rom[0x146]=3;
    c.game_rom[0x100]=0xc3; c.game_rom[0x101]=0x50; c.game_rom[0x102]=1;
    c.game_rom[0x150]=0xc3; c.game_rom[0x151]=0x50; c.game_rom[0x152]=1;
    const std::uint8_t boot[]{0x3e,0x80,0xe0,0x26,0x3e,0x77,0xe0,0x24,
        0x3e,0x11,0xe0,0x25,0x3e,0x80,0xe0,0x11,0x3e,0xf3,0xe0,0x12,
        0x3e,0xc0,0xe0,0x13,0x3e,0x87,0xe0,0x14,0x3e,0x91,0xe0,0x40,0xc3,0,1};
    std::copy(std::begin(boot),std::end(boot),c.gb_boot_rom.begin());
    c.spc_ipl[0]=0x2f; c.spc_ipl[1]=0xfe;
    return c;
}
std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    check(bool(file),"private input opens");
    const auto size=file.tellg();
    check(size>=0 && size<=1024*1024,"private input size bounded");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    check(bool(file),"private input read complete"); return bytes;
}
void hash_word(std::uint64_t& hash,std::uint64_t value,unsigned bytes) {
    for(unsigned n=0;n<bytes;++n) { hash^=static_cast<std::uint8_t>(value>>(8*n)); hash*=1099511628211ULL; }
}
void production_titles(const char* directory,const char* game,const char* save,const char* script_path) {
    const std::filesystem::path firmware=directory;
    check(!std::filesystem::exists(firmware/"sgb.boot.rom") &&
          !std::filesystem::exists(firmware/"sgb2.boot.rom"),"production fixture has no GB boot overrides");
    const auto bundled_ipl=gameboy::spc700_ipl_rom();
    const auto rom=read(game), battery=read(save);
    const auto ipl=std::filesystem::exists(firmware/"spc700.rom")?read(firmware/"spc700.rom"):
        std::vector<std::uint8_t>(bundled_ipl.begin(),bundled_ipl.end());
    check(ipl.size()==64,"production IPL size");
    gbb_sgb_input_script script{}; char error[128]{};
    check(gbb_sgb_input_load(script_path,&script,error,sizeof(error)),error);
    constexpr gbb::InputId input[]{gbb::InputId::right,gbb::InputId::left,gbb::InputId::up,gbb::InputId::down,
        gbb::InputId::a,gbb::InputId::b,gbb::InputId::select,gbb::InputId::start};
    for(const auto model:{gameboy::HardwareModel::sgb,gameboy::HardwareModel::sgb2}) {
        const bool sgb2=model==gameboy::HardwareModel::sgb2;
        gbb::CoreLoadOptions options; options.hardware_model=sgb2?"sgb2":"sgb";
        options.sgb_firmware_directory=firmware; // No source/persistence path: never write original saves.
        auto core=gbb::create_core(rom,options);
        check(core->descriptor().audio_sample_rate==48000 && core->descriptor().audio_channels==2,
              "production adapter uses stereo 48 kHz");
        core->import_persistent_data(gbb::PersistentDataKind::battery_ram,battery);
        gameboy::SgbHostConfig cfg; cfg.model=model; cfg.game_rom=rom; cfg.combined_audio=true; cfg.output_hz=48000;
        cfg.program_rom=read(firmware/(sgb2?"sgb2.program.rom":"sgb1.program.rom"));
        cfg.gb_boot_rom=sgb2?gameboy::sgb2_boot_rom():gameboy::sgb_boot_rom();
        std::copy(ipl.begin(),ipl.end(),cfg.spc_ipl.begin());
        gameboy::SgbHost oracle(cfg); oracle.import_battery_ram(battery);
        std::uint64_t audio_hash=14695981039346656037ULL, video_hash=audio_hash, samples=0, nonzero=0;
        unsigned restores=0; std::size_t next_input=0;
        const auto advance=[&](bool capture,bool restore=false) {
            while(!core->frame_ready()) {
                const auto before=oracle.cpu().timing().clocks();
                const auto clocks=core->step_instruction();
                check(oracle.step(),"production oracle instruction supported");
                check(clocks==oracle.cpu().timing().clocks()-before,"adapter/host instruction clocks exact");
            }
            if(restore) {
                check(oracle.pending_samples()>0,"production snapshot contains unread PCM");
                const auto snapshot=core->save_state();
                // Pause is frontend policy: queries must not clock the core.
                (void)core->video_frame(); (void)core->frame_ready();
                check(core->save_state()==snapshot,"paused queries cannot advance production state");
                (void)core->step_instruction(); core->load_state(snapshot);
                check(core->frame_ready() && core->save_state()==snapshot,
                      "production wrapper restores ready frame cursor and unread PCM"); ++restores;
            }
            const auto actual=core->take_audio_samples(); std::vector<std::int16_t> expected;
            gameboy::SgbHost::StereoSample sample;
            while(oracle.pop_sample(sample)) { expected.push_back(sample.left); expected.push_back(sample.right); }
            check(actual==expected,"production adapter PCM exact at every frame boundary");
            check(core->take_audio_samples().empty(),"production audio drains once");
            const auto view=core->video_frame(); const auto& pixels=oracle.icd().emulator().sgb_framebuffer();
            check(view.width==256 && view.height==224 && view.pixel_count==pixels.size() &&
                  std::equal(pixels.begin(),pixels.end(),view.pixels),"production border/viewport exact");
            if(capture) {
                for(auto value:actual) { hash_word(audio_hash,static_cast<std::uint16_t>(value),2); if(value) ++nonzero; }
                samples+=actual.size()/2;
                for(auto pixel:pixels) hash_word(video_hash,pixel,4);
            }
            core->consume_frame(); check(!core->frame_ready(),"production frame consumes once");
        };
        for(unsigned frame=0;frame<3600;++frame) {
            // Real frontends sample live input at presentation boundaries,
            // not at the raw host runner's native GB LCD-frame boundaries.
            if(next_input<script.count && script.events[next_input].frame==frame) {
                const auto mask=script.events[next_input++].mask;
                for(unsigned n=0;n<8;++n) {
                    core->set_input(input[n],(mask&(1U<<n))!=0);
                    oracle.set_button(static_cast<gameboy::Button>(n),(mask&(1U<<n))!=0);
                }
            }
            advance(true,frame%137==0);
            if(frame%120==0) {
                const auto state=core->save_state();
                check(std::vector<std::uint8_t>(state.begin()+16,state.end()-8)==oracle.save_state(),
                      "production wrapper owns exactly one matching host");
            }
        }
        check(next_input==script.count && !oracle.icd().emulator().bus().boot_rom_enabled(),"production replay completes input and bundled bootstrap");
        check(oracle.icd().sound_packets_delivered()>0 && samples>0 && nonzero>0,"production replay has SOUND delivery and audible digital output");
        std::uint64_t state_hash=14695981039346656037ULL;
        for(auto byte:oracle.icd().emulator().save_state()) hash_word(state_hash,byte,1);
        const auto live_ram=core->export_persistent_data(gbb::PersistentDataKind::battery_ram);
        core->reset(); oracle.reset(); oracle.import_battery_ram(live_ram);
        check(!core->frame_ready() && core->take_audio_samples().empty(),"production cold reset clears queued presentation");
        check(core->export_persistent_data(gbb::PersistentDataKind::battery_ram)==live_ram,"production reset keeps live battery RAM");
        for(unsigned n=0;n<64;++n) advance(false);
        const auto reset_state=core->save_state();
        check(std::vector<std::uint8_t>(reset_state.begin()+16,reset_state.end()-8)==oracle.save_state(),"production reset replays bundled startup exactly");
        std::cout<<"{\"format\":\"gbb-sgb-production-v1\",\"model\":\""<<(sgb2?"sgb2":"sgb1")
                 <<"\",\"boot\":\"bundled\",\"input_clock\":\"presentation-frame\",\"output_hz\":48000,\"frames\":3600,\"samples\":"<<samples
                 <<",\"nonzero\":"<<nonzero<<",\"audio_hash\":"<<audio_hash<<",\"video_hash\":"<<video_hash
                 <<",\"gb_state_hash\":"<<state_hash<<",\"restores\":"<<restores<<",\"inputs\":"<<next_input
#if defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
                 <<",\"pin_profile\":\"linux-gcc\""
#else
                 <<",\"pin_profile\":\"same-platform\""
#endif
                 <<"}\n"<<std::flush;
    }
}
}
int main(int argc,char** argv) {
    if(argc>1) {
        if(argc!=6 || std::string_view(argv[1])!="--local-production") return 2;
        try { production_titles(argv[2],argv[3],argv[4],argv[5]); return 0; }
        catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    }
    const auto root=std::filesystem::temp_directory_path()/
        ("gbb-firmware-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directory(root);
        auto cfg=fixture();
        write(root/"sgb1.program.rom",cfg.program_rom); write(root/"sgb2.program.rom",cfg.program_rom);
        write(root/"sgb.boot.rom",cfg.gb_boot_rom); write(root/"sgb2.boot.rom",cfg.gb_boot_rom);
        write(root/"spc700.rom",cfg.spc_ipl);
        gbb::CoreLoadOptions options; options.hardware_model="sgb2"; options.sgb_firmware_directory=root;
        auto core=gbb::create_core(cfg.game_rom,options);
        check(gbb::gameboy_emulator(core.get())==nullptr,"host GB never escapes through mutable legacy adapter");
        check(!gbb::has_capability(core->descriptor().capabilities,gbb::CoreCapability::link_cable),"link disabled");
        std::string error;
        check(gbb::validate_core_contract(*core,error),error.c_str());
        auto default_core=gbb::create_core(cfg.game_rom);
        check(gbb::gameboy_emulator(default_core.get())!=nullptr,"default HLE adapter unchanged");
        gameboy::SgbHost oracle(cfg);
        for(unsigned frame=0;frame<6;++frame) {
            const auto result=gbb::advance_to_frame(*core,core->descriptor().nominal_cycles_per_frame);
            check(result.frame_ready,"host cadence reaches frame during boot and running");
            std::vector<std::int16_t> expected;
            unsigned clocks{};
            while(clocks<result.cycles) {
                const auto before=oracle.cpu().timing().clocks();
                check(oracle.step(),"oracle steps");
                clocks+=static_cast<unsigned>(oracle.cpu().timing().clocks()-before);
            }
            gameboy::SgbHost::StereoSample sample;
            while(oracle.pop_sample(sample)) { expected.push_back(sample.left); expected.push_back(sample.right); }
            check(core->take_audio_samples()==expected,"combined audio exactly matches one host");
            check(core->take_audio_samples().empty(),"audio drains once");
            const auto snapshot=core->save_state();
            check(std::vector<std::uint8_t>(snapshot.begin()+16,snapshot.end()-8)==oracle.save_state(),"one GB/host state exactly matches oracle");
            check(core->video_frame().pixel_count==256*224,"single border framebuffer");
            const auto frame_view=core->video_frame();
            const auto& oracle_pixels=oracle.icd().emulator().sgb_framebuffer();
            check(std::equal(oracle_pixels.begin(),oracle_pixels.end(),frame_view.pixels),"video exactly matches sole host framebuffer");
            core->consume_frame(); check(!core->frame_ready(),"frame consumed once");
        }
        core->set_input(gbb::InputId::a,true);
        const auto held=core->save_state();
        core->set_input(gbb::InputId::a,false);
        core->load_state(held); check(core->save_state()==held,"live input and frame cursor restore");
        check(oracle.load_state(std::vector<std::uint8_t>(held.begin()+16,held.end()-8)),"input snapshot loads into host");
        oracle.set_button(gameboy::Button::a,false);
        core->set_input(gbb::InputId::a,false);
        const auto released=core->save_state();
        check(std::vector<std::uint8_t>(released.begin()+16,released.end()-8)==oracle.save_state(),"live release forwarded to sole host");
        core->load_state(held);
        auto broken=held; broken[8]^=1;
        rejects([&]{core->load_state(broken);},"corrupt wrapper rejected");
        check(core->save_state()==held,"failed snapshot load transactional");
        broken=held;
        const std::uint64_t future=std::uint64_t(core->descriptor().nominal_cycles_per_frame)*1000000;
        for(unsigned n=0;n<8;++n) broken[8+n]=static_cast<std::uint8_t>(future>>(n*8));
        std::uint64_t checksum=14695981039346656037ULL;
        for(std::size_t n=0;n<broken.size()-8;++n) { checksum^=broken[n]; checksum*=1099511628211ULL; }
        for(unsigned n=0;n<8;++n) broken[broken.size()-8+n]=static_cast<std::uint8_t>(checksum>>(n*8));
        rejects([&]{core->load_state(broken);},"checksum-valid impossible cursor rejected");
        check(core->save_state()==held,"invalid cursor rollback transactional");
        core->reset(); check(!core->frame_ready() && core->take_audio_samples().empty(),"reset clears unread audio/cadence");
        check(gbb::advance_to_frame(*core,core->descriptor().nominal_cycles_per_frame).frame_ready,"reset resumes");
        options.hardware_model="sgb";
        auto sgb1=gbb::create_core(cfg.game_rom,options);
        auto sgb1_config=cfg; sgb1_config.model=gameboy::HardwareModel::sgb;
        gameboy::SgbHost sgb1_oracle(sgb1_config);
        const auto sgb1_result=gbb::advance_to_frame(*sgb1,sgb1->descriptor().nominal_cycles_per_frame);
        check(sgb1_result.frame_ready,"SGB1 firmware frame cadence");
        unsigned sgb1_clocks{};
        while(sgb1_clocks<sgb1_result.cycles) {
            const auto before=sgb1_oracle.cpu().timing().clocks();
            check(sgb1_oracle.step(),"SGB1 oracle steps");
            sgb1_clocks+=static_cast<unsigned>(sgb1_oracle.cpu().timing().clocks()-before);
        }
        const auto sgb1_snapshot=sgb1->save_state();
        check(std::vector<std::uint8_t>(sgb1_snapshot.begin()+16,sgb1_snapshot.end()-8)==sgb1_oracle.save_state(),"SGB1 adapter exactly matches host");
        rejects([&]{sgb1->load_state(held);},"cross-model snapshot rejected");
        check(sgb1->save_state()==sgb1_snapshot,"cross-model failure transactional");
        auto faulty=cfg.program_rom; faulty[0x104]=0xff;
        write(root/"sgb1.program.rom",faulty);
        auto fault_core=gbb::create_core(cfg.game_rom,options);
        rejects([&]{static_cast<void>(fault_core->step_instruction());},"unsupported host opcode fails closed");
        rejects([&]{static_cast<void>(fault_core->step_instruction());},"host fault remains terminal");
        write(root/"sgb1.program.rom",cfg.program_rom);
        options.hardware_model="auto";
        rejects([&]{static_cast<void>(gbb::create_core(cfg.game_rom,options));},"explicit model required");
        options.hardware_model="sgb2";
        auto battery=cfg.game_rom; battery[0x147]=3; battery[0x149]=2;
        options.source_path=root/"game.gb";
        auto persistent=gbb::create_core(battery,options);
        auto ram=persistent->export_persistent_data(gbb::PersistentDataKind::battery_ram);
        check(ram.size()==8192,"battery RAM size"); ram[0]=42;
        persistent->import_persistent_data(gbb::PersistentDataKind::battery_ram,ram);
        persistent->flush_persistent_data();
        check(std::filesystem::exists(root/"game.sgb2-firmware.sav") && !std::filesystem::exists(root/"game.sav"),"experimental save isolated");
        auto reloaded=gbb::create_core(battery,options);
        check(reloaded->export_persistent_data(gbb::PersistentDataKind::battery_ram)==ram,"experimental save reload");
        auto newer=ram; newer[0]=43;
        persistent->import_persistent_data(gbb::PersistentDataKind::battery_ram,newer);
        const auto lock=root/"game.sgb2-firmware.sav.pending";
        std::filesystem::create_directory(lock);
        rejects([&]{persistent->flush_persistent_data();},"occupied staging directory rejected");
        auto unchanged=gbb::create_core(battery,options);
        check(unchanged->export_persistent_data(gbb::PersistentDataKind::battery_ram)==ram,"failed save preserves previous file");
        std::filesystem::remove(lock);
        persistent->flush_persistent_data();
        ram=newer;
        persistent->reset(); check(persistent->export_persistent_data(gbb::PersistentDataKind::battery_ram)==ram,"reset preserves live save");
        write(root/"spc700.rom",std::vector<std::uint8_t>(63));
        rejects([&]{static_cast<void>(gbb::create_core(cfg.game_rom,options));},"wrong IPL size rejected");
        char app[]="gbb",rom[]="game.gb",flag[]="--sgb-firmware",dir[]="firmware",model_flag[]="--sgb-model",model[]="sgb";
        char* args[]{app,rom,flag,dir,model_flag,model};
        const auto launch=gbb::sdl::desktop_launch_options(6,args);
        check(launch.firmware_model=="sgb" && launch.firmware_directory=="firmware","desktop opt-in parsed");
        check(gbb::sdl::desktop_launch_options(2,args).firmware_directory.empty(),"ordinary launch unchanged");
        rejects([&]{gbb::sdl::desktop_launch_options(3,args);},"missing launch value rejected");
        gbb::sdl::DesktopFirmwareSettings pending;
        check(gbb::sdl::desktop_core_load_options(pending,"auto").sgb_firmware_directory.empty(),"HLE remains default");
        pending={true,root,"sgb2"};
        rejects([&]{gbb::sdl::validate_desktop_firmware(pending);},"settings validate wrong-size IPL before launch");
        write(root/"spc700.rom",std::vector<std::uint8_t>(cfg.spc_ipl.begin(),cfg.spc_ipl.end()));
        gbb::sdl::validate_desktop_firmware(pending);
        auto invalid_settings=pending;
        invalid_settings.model="auto";
        rejects([&]{gbb::sdl::validate_desktop_firmware(invalid_settings);},"invalid persisted model does not fall back to HLE");
        invalid_settings=pending;
        invalid_settings.directory=root/"missing";
        rejects([&]{gbb::sdl::validate_desktop_firmware(invalid_settings);},"missing firmware directory rejected before launch");
        const auto firmware_launch=gbb::sdl::desktop_core_load_options(pending,"auto");
        check(firmware_launch.hardware_model=="sgb2" && firmware_launch.sgb_firmware_directory==root,"persisted settings choose firmware and explicit model");
        const auto override_launch=gbb::sdl::desktop_core_load_options(pending,"auto",{true,root,"sgb"});
        check(override_launch.hardware_model=="sgb","explicit CLI overrides persisted settings");
        auto settings_core=gbb::create_core(cfg.game_rom,firmware_launch);
        check(gbb::gameboy_emulator(settings_core.get())==nullptr,"settings launch owns firmware core");
        pending.enabled=false;
        const auto hle_launch=gbb::sdl::desktop_core_load_options(pending,"auto");
        auto hle_core=gbb::create_core(cfg.game_rom,hle_launch);
        check(gbb::gameboy_emulator(hle_core.get())!=nullptr,"disabled setting returns next launch to HLE");
        check(gbb::gameboy_emulator(settings_core.get())==nullptr,"settings change does not replace running firmware core");
        check(!gbb::sdl::firmware_video_supported(gameboy::VideoMode::voxel_diorama) &&
              !gbb::sdl::firmware_video_supported(gameboy::VideoMode::voxel_shape) &&
              !gbb::sdl::firmware_video_supported(gameboy::VideoMode::voxel_popup) &&
              gbb::sdl::firmware_video_supported(gameboy::VideoMode::nearest),"all unsupported voxel modes excluded without changing 2D visuals");
        char smoke_flag[]="--frontend-smoke-frames", smoke_count[]="36000", excessive[]="36001";
        char* preference_smoke[]{app,rom,smoke_flag,smoke_count};
        check(gbb::sdl::desktop_launch_options(4,preference_smoke).firmware_directory.empty(),"bounded preferences smoke does not override backend");
        char* model_without_directory[]{app,rom,model_flag,model};
        rejects([&]{gbb::sdl::desktop_launch_options(4,model_without_directory);},"model-only override still rejected");
        char* smoke_args[]{app,rom,flag,dir,smoke_flag,smoke_count};
        check(gbb::sdl::desktop_launch_options(6,smoke_args).smoke_frames==36000,"bounded ten-minute qualification parsed");
        smoke_args[5]=excessive;
        rejects([&]{gbb::sdl::desktop_launch_options(6,smoke_args);},"unbounded qualification rejected");
        // Both boot images are optional; private SNES program images stay required.
        std::filesystem::remove(root/"sgb.boot.rom");
        std::filesystem::remove(root/"sgb2.boot.rom");
        std::filesystem::remove(root/"spc700.rom");
        // An original audible cartridge fixture, not a private game dump.
        auto audible=cfg.game_rom;
        std::copy_n(cfg.gb_boot_rom.begin(),35,audible.begin()+0x150);
        audible[0x150+33]=0x70; audible[0x150+34]=1; // JP to its own final instruction.
        for (const auto name : {"sgb", "sgb2"}) {
            gbb::validate_sgb_firmware_images(root,name);
            options.hardware_model=name;
            auto bundled=gbb::create_core(audible,options);
            auto reference_cfg=cfg; reference_cfg.game_rom=audible;
            reference_cfg.spc_ipl=gameboy::spc700_ipl_rom();
            reference_cfg.model=std::string_view(name)=="sgb2"?gameboy::HardwareModel::sgb2:gameboy::HardwareModel::sgb;
            reference_cfg.gb_boot_rom=reference_cfg.model==gameboy::HardwareModel::sgb2?
                gameboy::sgb2_boot_rom():gameboy::sgb_boot_rom();
            gameboy::SgbHost reference(reference_cfg);
            bool nonzero=false;
            for(unsigned frame=0;frame<64;++frame) {
                while(!bundled->frame_ready()) {
                    const auto before=reference.cpu().timing().clocks();
                    check(reference.step(),"synthetic bundled oracle step");
                    check(bundled->step_instruction()==reference.cpu().timing().clocks()-before,"bundled instruction timing exact");
                }
                if(frame%17==0) {
                    check(reference.pending_samples()>0,"ROM-free snapshot has unread PCM");
                    const auto queued=bundled->save_state();
                    (void)bundled->step_instruction(); bundled->load_state(queued);
                    check(bundled->frame_ready() && bundled->save_state()==queued,"bundled ready-frame and unread PCM restore exactly");
                }
                std::vector<std::int16_t> expected;
                gameboy::SgbHost::StereoSample sample;
                while(reference.pop_sample(sample)) { expected.push_back(sample.left); expected.push_back(sample.right); }
                check(bundled->take_audio_samples()==expected,"bundled 48 kHz stereo PCM exact");
                nonzero|=std::any_of(expected.begin(),expected.end(),[](auto value){return value!=0;});
                const auto view=bundled->video_frame(); const auto& pixels=reference.icd().emulator().sgb_framebuffer();
                check(view.pixel_count==pixels.size() && std::equal(pixels.begin(),pixels.end(),view.pixels),"bundled full viewport exact");
                bundled->consume_frame();
            }
            check(!reference.icd().emulator().bus().boot_rom_enabled() && nonzero,"bundled bootstrap hands off to audible cartridge");
            const auto state=bundled->save_state(); bundled->load_state(state);
            check(bundled->save_state()==state,"bundled firmware snapshot roundtrip");
            bundled->reset();
            check(gbb::advance_to_frame(*bundled,bundled->descriptor().nominal_cycles_per_frame).frame_ready,
                  "bundled firmware reset resumes");
        }
        write(root/"sgb2.boot.rom",std::array<std::uint8_t,1>{0});
        rejects([&]{gbb::validate_sgb_firmware_images(root,"sgb2");},"malformed boot override does not fall back");
        std::filesystem::remove_all(root); return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n'; std::filesystem::remove_all(root); return 1;
    }
}
