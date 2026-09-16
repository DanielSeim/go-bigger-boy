#pragma once

#ifdef _WIN32

namespace gbb_desktop {

// Runs the native dashboard through a small Win32 message-loop smoke test.
// This is invoked only by the Windows CTest entry point and never by normal
// application startup.
[[nodiscard]] int run_windows_dashboard_smoke();

} // namespace gbb_desktop

#endif
