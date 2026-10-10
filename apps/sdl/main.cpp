#include <SDL3/SDL_main.h>
#include "windows_taskbar.hpp"

int run_emulation(int argc, char** argv);

int main(int argc, char** argv) {
#ifdef _WIN32
    gbb_desktop::initialize_windows_taskbar();
#endif
    return run_emulation(argc, argv);
}
