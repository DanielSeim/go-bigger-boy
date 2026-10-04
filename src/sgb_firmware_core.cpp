#include "gbb/sgb_firmware_core.hpp"
#include "gbb/gameboy_scene.hpp"
#include "gbb/log.hpp"
#include "gameboy/sgb_host.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace gbb {
namespace {
constexpr unsigned frame_clocks = 1364 * 262;
constexpr std::uint64_t master_hz = 21477273;
constexpr std::array<InputDescriptor, 8> inputs{{
    {InputId::right,"Right"}, {InputId::left,"Left"}, {InputId::up,"Up"},
    {InputId::down,"Down"}, {InputId::a,"A"}, {InputId::b,"B"},
    {InputId::select,"Select"}, {InputId::start,"Start"}}};

std::vector<std::uint8_t> read_image(const std::filesystem::path& path,
                                    std::size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Could not open caller-owned image: " + path.string());
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > limit)
        throw std::runtime_error("Invalid image size: " + path.string());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read complete image: " + path.string());
    return bytes;
}
template<std::size_t N>
std::array<std::uint8_t,N> fixed_image(const std::filesystem::path& path) {
    const auto bytes = read_image(path,N);
    if (bytes.size() != N) throw std::runtime_error("Wrong firmware image size: " + path.string());
    std::array<std::uint8_t,N> result{};
    std::copy(bytes.begin(),bytes.end(),result.begin());
    return result;
}
std::uint64_t hash(const std::vector<std::uint8_t>& bytes, std::size_t size) {
    std::uint64_t h=14695981039346656037ULL;
    for (std::size_t n=0;n<size;++n) { h^=bytes[n]; h*=1099511628211ULL; }
    return h;
}
void append64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for(unsigned n=0;n<8;++n) bytes.push_back(static_cast<std::uint8_t>(value>>(n*8)));
}
std::uint64_t read64(const std::vector<std::uint8_t>& bytes,std::size_t offset) {
    std::uint64_t result{};
    for(unsigned n=0;n<8;++n) result|=std::uint64_t(bytes[offset+n])<<(n*8);
    return result;
}

class FirmwareCore final : public EmulatorCore {
public:
    FirmwareCore(gameboy::SgbHostConfig config, std::filesystem::path save)
        : host_(std::move(config)), save_path_(std::move(save)) {
        const auto& gb=host_.icd().emulator();
        // These cartridge peripherals have not been integrated into this path.
        if (gb.has_rtc() || gb.has_camera() || gb.has_rumble() || gb.bus().cartridge().requires_cgb())
            throw std::invalid_argument("Experimental SGB firmware playback does not support RTC, camera, rumble or CGB-only cartridges");
        title_=gb.bus().cartridge().title();
        descriptor_.software_title=title_;
        descriptor_.rom_size=gb.bus().cartridge().rom_size();
        descriptor_.save_ram_size=gb.bus().cartridge().ram_size();
        descriptor_.has_battery=gb.has_battery();
        descriptor_.supports_color=gb.bus().cartridge().supports_cgb();
        if (gb.has_battery()) descriptor_.capabilities=descriptor_.capabilities | CoreCapability::persistent_memory;
        if (gb.has_battery() && !save_path_.empty() && std::filesystem::exists(save_path_))
            import_persistent_data(PersistentDataKind::battery_ram,
                                   read_image(save_path_, descriptor_.save_ram_size));
        last_saved_ram_=gb.export_battery_ram();
    }
    ~FirmwareCore() override {
        try { flush_persistent_data(); }
        catch(const std::exception& error) {
            try { Logger::instance().write(LogLevel::error,LogCategory::core,error.what()); } catch(...) {}
        }
    }
    std::string_view model() const noexcept {
        return host_.icd().emulator().hardware_model()==gameboy::HardwareModel::sgb2 ? "sgb2" : "sgb";
    }
    const CoreDescriptor& descriptor() const noexcept override { return descriptor_; }
    void reset() noexcept override {
        try {
            const auto ram=host_.icd().emulator().export_battery_ram();
            host_.reset();
            if (descriptor_.has_battery && !ram.empty()) host_.import_battery_ram(ram);
            next_frame_=frame_clocks; reset_failed_=false;
        } catch (...) { reset_failed_=true; }
    }
    unsigned step_instruction() override {
        if (reset_failed_) throw std::runtime_error("SGB firmware reset failed");
        const auto before=host_.cpu().timing().clocks();
        if (!host_.step()) {
            const auto fault=host_.fault();
            throw std::runtime_error("SGB firmware playback stopped: status=" +
                std::to_string(static_cast<unsigned>(host_.status())) +
                " address=" + std::to_string(fault.address));
        }
        return static_cast<unsigned>(host_.cpu().timing().clocks()-before);
    }
    bool frame_ready() const noexcept override { return host_.cpu().timing().clocks()>=next_frame_; }
    void consume_frame() noexcept override {
        if (frame_ready()) next_frame_=host_.cpu().timing().clocks()/frame_clocks*frame_clocks+frame_clocks;
    }
    VideoFrameView video_frame() const noexcept override {
        const auto& pixels=host_.icd().emulator().sgb_framebuffer();
        return {pixels.data(),pixels.size(),gameboy::Ppu::sgb_border_width,
                gameboy::Ppu::sgb_border_height,gameboy::Ppu::sgb_border_width*sizeof(std::uint32_t)};
    }
    bool video_frame_native_colors() const noexcept override { return true; }
    const SceneSnapshot& scene_snapshot() const noexcept override {
        populate_gameboy_scene_snapshot(host_.icd().emulator(),scene_); return scene_;
    }
    std::vector<std::int16_t> take_audio_samples() override {
        std::vector<std::int16_t> result;
        result.reserve(host_.pending_samples()*2);
        gameboy::SgbHost::StereoSample sample;
        while(host_.pop_sample(sample)) { result.push_back(sample.left); result.push_back(sample.right); }
        return result;
    }
    void set_input(InputId input,bool pressed) noexcept override {
        for (unsigned n=0;n<inputs.size();++n)
            if (inputs[n].id==input) host_.set_button(static_cast<gameboy::Button>(n),pressed);
    }
    std::vector<std::uint8_t> save_state() const override {
        auto state=host_.save_state();
        std::vector<std::uint8_t> result{'G','B','B','F','W','0','0','1'};
        append64(result,next_frame_);
        result.insert(result.end(),state.begin(),state.end());
        append64(result,hash(result,result.size())); return result;
    }
    void load_state(const std::vector<std::uint8_t>& state) override {
        constexpr std::array<std::uint8_t,8> magic{'G','B','B','F','W','0','0','1'};
        if(state.size()<24 || state.size()>16*1024*1024+24 ||
           !std::equal(magic.begin(),magic.end(),state.begin()) ||
           read64(state,state.size()-8)!=hash(state,state.size()-8))
            throw std::runtime_error("Invalid experimental SGB firmware snapshot");
        const auto cursor=read64(state,8);
        if(!cursor || cursor%frame_clocks || cursor>std::numeric_limits<std::uint64_t>::max()-frame_clocks)
            throw std::runtime_error("Invalid SGB firmware frame cursor");
        const std::vector<std::uint8_t> payload(state.begin()+16,state.end()-8);
        const auto previous=host_.save_state();
        if(!host_.load_state(payload)) throw std::runtime_error("SGB firmware snapshot identity or state mismatch");
        const auto clock=host_.cpu().timing().clocks();
        if(clock>std::numeric_limits<std::uint64_t>::max()-frame_clocks || cursor>clock+frame_clocks) {
            if(!host_.load_state(previous)) reset_failed_=true;
            throw std::runtime_error("SGB firmware snapshot frame cursor is ahead of the host");
        }
        next_frame_=cursor; reset_failed_=false;
    }
    std::uint64_t rom_fingerprint() const noexcept override { return host_.icd().emulator().rom_fingerprint(); }
    bool has_persistent_data(PersistentDataKind kind) const noexcept override {
        return kind!=PersistentDataKind::rtc && descriptor_.has_battery;
    }
    std::vector<std::uint8_t> export_persistent_data(PersistentDataKind kind) const override {
        return has_persistent_data(kind) ? host_.icd().emulator().export_battery_ram() : std::vector<std::uint8_t>{};
    }
    void import_persistent_data(PersistentDataKind kind,const std::vector<std::uint8_t>& bytes) override {
        if(!has_persistent_data(kind) || bytes.size()!=descriptor_.save_ram_size)
            throw std::invalid_argument("Invalid SGB firmware battery RAM");
        host_.import_battery_ram(bytes);
    }
    void flush_persistent_data() override {
        if (!descriptor_.has_battery || save_path_.empty()) return;
        const auto bytes=host_.icd().emulator().export_battery_ram();
        if(bytes==last_saved_ram_) return;
        // Create an exclusive sibling staging directory. A failed write or
        // replacement must never truncate the last good experimental save.
        auto staging=save_path_; staging += ".pending";
        if(!std::filesystem::create_directory(staging))
            throw std::runtime_error("SGB save staging directory already exists: " + staging.string());
        const auto temporary=staging/"ram";
        try {
            {
                std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
                file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
                file.flush();
                if(!file) throw std::runtime_error("Could not write experimental SGB battery RAM");
            }
#ifdef _WIN32
            if(!MoveFileExW(temporary.c_str(),save_path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Could not replace experimental SGB battery RAM");
#else
            std::filesystem::rename(temporary,save_path_);
#endif
            std::filesystem::remove(staging);
        } catch(...) {
            std::error_code ignored;
            std::filesystem::remove(temporary,ignored);
            std::filesystem::remove(staging,ignored);
            throw;
        }
        last_saved_ram_=bytes;
    }
private:
    gameboy::SgbHost host_;
    std::filesystem::path save_path_;
    std::vector<std::uint8_t> last_saved_ram_;
    std::string title_;
    mutable SceneSnapshot scene_;
    std::uint64_t next_frame_{frame_clocks};
    bool reset_failed_{};
    CoreDescriptor descriptor_{"gb","Game Boy / Game Boy Color",SystemId::game_boy,
        gameboy::Ppu::sgb_border_width,gameboy::Ppu::sgb_border_height,
        double(master_hz)/frame_clocks,double(master_hz),frame_clocks,48000,2,
        inputs.data(),inputs.size(),CoreCapability::scene_layers};
};
}

std::unique_ptr<EmulatorCore> create_sgb_firmware_core(
    std::vector<std::uint8_t> rom,const CoreLoadOptions& options) {
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    static_cast<void>(rom); static_cast<void>(options);
    throw std::invalid_argument("Experimental firmware playback is desktop-only");
#else
    if(options.hardware_model!="sgb" && options.hardware_model!="sgb2")
        throw std::invalid_argument("Firmware playback requires an explicit sgb or sgb2 model");
    gameboy::SgbHostConfig config;
    const bool sgb2=options.hardware_model=="sgb2";
    config.model=sgb2?gameboy::HardwareModel::sgb2:gameboy::HardwareModel::sgb;
    config.game_rom=std::move(rom); config.combined_audio=true; config.output_hz=48000;
    const auto& dir=options.sgb_firmware_directory;
    config.program_rom=read_image(dir/(sgb2?"sgb2.program.rom":"sgb1.program.rom"),0x80000);
    config.gb_boot_rom=fixed_image<256>(dir/(sgb2?"sgb2.boot.rom":"sgb.boot.rom"));
    config.spc_ipl=fixed_image<64>(dir/"spc700.rom");
    auto save=options.persistence_path.empty()?options.source_path:options.persistence_path;
    if(!save.empty()) save.replace_extension(sgb2?".sgb2-firmware.sav":".sgb-firmware.sav");
    return std::make_unique<FirmwareCore>(std::move(config),std::move(save));
#endif
}
std::string_view sgb_firmware_model(const EmulatorCore& core) noexcept {
    const auto* adapter=dynamic_cast<const FirmwareCore*>(&core);
    return adapter ? adapter->model() : std::string_view{};
}
}
