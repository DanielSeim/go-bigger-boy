#pragma once

#ifdef _WIN32

#include <windows.h>

namespace gbb_desktop {

inline constexpr UINT windows_dashboard_smoke_close = WM_APP + 17;

// Runs the native dashboard through a small Win32 message-loop smoke test.
// This is invoked only by the Windows CTest entry point and never by normal
// application startup.
[[nodiscard]] int run_windows_dashboard_smoke();

} // namespace gbb_desktop

#endif
