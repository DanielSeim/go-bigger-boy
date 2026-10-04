#include "gbb/core_registry.hpp"
#include "gbb/core_runtime.hpp"
#include "gbb/gameboy_core.hpp"
#include "gbb/core_contract.hpp"
#include "gameboy/sgb_host.hpp"
#include "desktop_launch_options.hpp"
#include "desktop_firmware_settings.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

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
}
int main() {
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
        std::filesystem::remove_all(root); return 0;
    } catch(const std::exception& e) {
        std::cerr<<e.what()<<'\n'; std::filesystem::remove_all(root); return 1;
    }
}
