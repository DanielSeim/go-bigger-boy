#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gbb::sdl {
struct DesktopLaunchOptions {
    std::filesystem::path firmware_directory;
    std::string firmware_model{"sgb2"};
    unsigned smoke_frames{};
};
inline DesktopLaunchOptions desktop_launch_options(int argc,char** argv) {
    DesktopLaunchOptions result;
    for(int n=2;n<argc;++n) {
        const std::string_view option=argv[n];
        if(n+1==argc) throw std::invalid_argument("Missing desktop launch option value");
        const std::string value=argv[++n];
        if(option=="--sgb-firmware") {
            if(value.empty() || !result.firmware_directory.empty()) throw std::invalid_argument("Invalid or duplicate firmware directory");
            result.firmware_directory=std::filesystem::u8path(value);
        } else if(option=="--sgb-model") {
            if(value!="sgb" && value!="sgb2") throw std::invalid_argument("Firmware model must be sgb or sgb2");
            result.firmware_model=value;
        } else if(option=="--frontend-smoke-frames") {
            std::size_t consumed{};
            const auto frames=std::stoul(value,&consumed);
            if(consumed!=value.size() || frames<1 || frames>1800) throw std::invalid_argument("Smoke frame count must be 1..1800");
            result.smoke_frames=static_cast<unsigned>(frames);
        } else throw std::invalid_argument("Unknown desktop launch option: " + std::string(option));
    }
    if(argc>2 && result.firmware_directory.empty()) throw std::invalid_argument("Experimental launch options require --sgb-firmware DIRECTORY");
    if(argc>2 && std::string_view(argv[1]).substr(0,2)=="--") throw std::invalid_argument("Specify the game ROM before experimental options");
    return result;
}
}
