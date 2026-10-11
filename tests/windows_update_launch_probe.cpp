#include "update_checker.hpp"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <iostream>

// Exercise the production launcher in a process that can exit while the
// updater waits for it. The same binary stands in for the relaunched app.
int main() {
    int count = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 2;
    if (count == 1) {
        LocalFree(arguments);
        std::ifstream settings("settings.ini", std::ios::binary);
        std::ofstream marker("restarted.txt", std::ios::binary);
        marker << settings.rdbuf();
        return marker ? 0 : 3;
    }
    if (count != 5) {
        LocalFree(arguments);
        return 2;
    }
    gbb_desktop::DownloadedUpdate update;
    update.archive = std::filesystem::path(arguments[1]);
    const std::filesystem::path root(arguments[2]);
    const std::filesystem::path executable(arguments[3]);
    const std::wstring digest(arguments[4]);
    update.release.sha256.assign(digest.begin(), digest.end());
    LocalFree(arguments);
    std::string error;
    if (!gbb_desktop::launch_update_installer(update, root, executable, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    // Leave the installed executable locked briefly to exercise parent wait.
    Sleep(250);
    return 0;
}
