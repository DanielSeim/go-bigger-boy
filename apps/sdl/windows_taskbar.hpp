#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace gbb_desktop {

void initialize_windows_taskbar();
void configure_windows_taskbar(HWND window);
void clear_windows_taskbar(HWND window);

} // namespace gbb_desktop
#endif
