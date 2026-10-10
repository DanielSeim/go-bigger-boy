#include "windows_taskbar.hpp"
#include "resource.h"

#include <SDL3/SDL.h>
#include <shobjidl.h>
#include <propkey.h>

#include <string>
#include <vector>

namespace gbb_desktop {
namespace {

// Keep the ID independent of the version so upgrades reuse the pinned button.
constexpr wchar_t app_id[] = L"DanielSeim.GoBiggerBoy";

HRESULT set_string(IPropertyStore* store, REFPROPERTYKEY key,
                   const wchar_t* text) {
    PROPVARIANT value{};
    value.vt = VT_LPWSTR;
    // SetValue copies the string; this variant borrows it only for the call.
    value.pwszVal = const_cast<wchar_t*>(text);
    return store->SetValue(key, value);
}

void report_failure(const HRESULT result) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Could not configure Windows taskbar identity (0x%08lx)",
                static_cast<unsigned long>(result));
}

} // namespace

void initialize_windows_taskbar() {
    SDL_SetHint(SDL_HINT_APP_ID, "DanielSeim.GoBiggerBoy");
    const auto result = SetCurrentProcessExplicitAppUserModelID(app_id);
    if (FAILED(result)) report_failure(result);
}

void configure_windows_taskbar(HWND window) {
    if (window == nullptr) return;
    std::vector<wchar_t> buffer(32768);
    const auto size = GetModuleFileNameW(nullptr, buffer.data(),
                                        static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) {
        report_failure(HRESULT_FROM_WIN32(size == 0 ? GetLastError()
                                                    : ERROR_INSUFFICIENT_BUFFER));
        return;
    }
    const std::wstring executable(buffer.data(), size);
    const auto command = L"\"" + executable + L"\"";
    const auto display_name = L"@" + executable + L",-" +
                              std::to_wstring(IDS_GBB_TASKBAR_NAME);
    const auto icon = executable + L",0";
    IPropertyStore* store = nullptr;
    auto result = SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store));
    if (FAILED(result)) {
        report_failure(result);
        return;
    }
    // Set the relaunch properties together, before publishing the window ID.
    result = set_string(store, PKEY_AppUserModel_RelaunchCommand,
                        command.c_str());
    if (SUCCEEDED(result)) {
        result = set_string(store, PKEY_AppUserModel_RelaunchDisplayNameResource,
                            display_name.c_str());
    }
    if (SUCCEEDED(result)) {
        result = set_string(store, PKEY_AppUserModel_RelaunchIconResource,
                            icon.c_str());
    }
    if (SUCCEEDED(result)) result = set_string(store, PKEY_AppUserModel_ID, app_id);
    store->Release();
    if (FAILED(result)) report_failure(result);
}

void clear_windows_taskbar(HWND window) {
    if (window == nullptr) return;
    IPropertyStore* store = nullptr;
    const auto result = SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store));
    if (FAILED(result)) {
        report_failure(result);
        return;
    }
    // Window property values must be released before the HWND is destroyed.
    const PROPVARIANT empty{};
    for (const auto* key : {&PKEY_AppUserModel_ID,
                            &PKEY_AppUserModel_RelaunchCommand,
                            &PKEY_AppUserModel_RelaunchDisplayNameResource,
                            &PKEY_AppUserModel_RelaunchIconResource}) {
        const auto cleared = store->SetValue(*key, empty);
        if (FAILED(cleared)) report_failure(cleared);
    }
    store->Release();
}

} // namespace gbb_desktop
