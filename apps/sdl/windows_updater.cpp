#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Handle {
    HANDLE value{};
    ~Handle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::wstring quote_argument(const std::wstring& value) {
    std::wstring result{L"\""};
    std::size_t backslashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'\"';
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            result += character;
            backslashes = 0;
        }
    }
    result.append(backslashes * 2, L'\\');
    result += L'\"';
    return result;
}

bool run_process(const std::wstring& application,
                 const std::wstring& command_line,
                 const bool wait, std::wstring& error) {
    std::vector<wchar_t> mutable_command(command_line.begin(),
                                          command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.empty() ? nullptr : application.c_str(),
                        mutable_command.data(), nullptr, nullptr, FALSE,
                        wait ? CREATE_NO_WINDOW : CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        error = L"Could not start the Windows update helper process.";
        return false;
    }
    Handle thread{process.hThread};
    Handle child{process.hProcess};
    if (!wait) return true;
    if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0) {
        error = L"The update helper process did not finish.";
        return false;
    }
    DWORD status = 1;
    if (!GetExitCodeProcess(child.value, &status) || status != 0) {
        error = L"The update helper could not install the downloaded release.";
        return false;
    }
    return true;
}

bool wait_for_parent(const DWORD process_id, std::wstring& error) {
    Handle parent{OpenProcess(SYNCHRONIZE, FALSE, process_id)};
    if (parent.value == nullptr) {
        // ERROR_INVALID_PARAMETER means the process has already exited.
        if (GetLastError() == ERROR_INVALID_PARAMETER) return true;
        error = L"Could not wait for the running Go Bigger Boy process.";
        return false;
    }
    if (WaitForSingleObject(parent.value, INFINITE) != WAIT_OBJECT_0) {
        error = L"Could not wait for the running Go Bigger Boy process.";
        return false;
    }
    return true;
}

bool sha256_file(const std::filesystem::path& path, std::wstring& digest,
                 std::wstring& error) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
        error = L"Could not initialize SHA-256 verification.";
        return false;
    }

    DWORD object_size = 0;
    DWORD result_size = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &result_size, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&result_size),
                          sizeof(result_size), &result_size, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"Could not initialize SHA-256 verification.";
        return false;
    }
    std::vector<UCHAR> object(object_size);
    std::vector<UCHAR> hash(result_size);
    BCRYPT_HASH_HANDLE state{};
    if (BCryptCreateHash(algorithm, &state, object.data(), object_size,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = L"Could not initialize SHA-256 verification.";
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    std::vector<char> buffer(64 * 1024);
    bool success = static_cast<bool>(input);
    while (success && input.read(buffer.data(), buffer.size())) {
        success = BCryptHashData(
                      state, reinterpret_cast<PUCHAR>(buffer.data()),
                      static_cast<ULONG>(input.gcount()), 0) >= 0;
    }
    if (success && input.gcount() > 0) {
        success = BCryptHashData(
                      state, reinterpret_cast<PUCHAR>(buffer.data()),
                      static_cast<ULONG>(input.gcount()), 0) >= 0;
    }
    if (success) {
        success = BCryptFinishHash(state, hash.data(),
                                    static_cast<ULONG>(hash.size()), 0) >= 0;
    }
    BCryptDestroyHash(state);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) {
        error = L"Could not verify the downloaded release archive.";
        return false;
    }

    static constexpr wchar_t digits[] = L"0123456789abcdef";
    digest.clear();
    digest.reserve(hash.size() * 2);
    for (const auto byte : hash) {
        digest += digits[byte >> 4];
        digest += digits[byte & 0x0f];
    }
    return true;
}

bool equals_case_insensitive(std::wstring left, std::wstring right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        left[index] = static_cast<wchar_t>(towlower(left[index]));
        right[index] = static_cast<wchar_t>(towlower(right[index]));
    }
    return left == right;
}

bool extract_archive(const std::filesystem::path& archive,
                     const std::filesystem::path& staging,
                     std::wstring& error) {
    std::error_code ignored;
    std::filesystem::remove_all(staging, ignored);
    std::filesystem::create_directories(staging, ignored);
    if (ignored) {
        error = L"Could not create the update staging directory.";
        return false;
    }

    wchar_t system_directory[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system_directory,
                                            static_cast<UINT>(std::size(
                                                system_directory)));
    const std::filesystem::path tar = length == 0
        ? std::filesystem::path{L"tar.exe"}
        : std::filesystem::path{system_directory} / L"tar.exe";
    const auto command = quote_argument(tar.wstring()) + L" -xf " +
                         quote_argument(archive.wstring()) + L" -C " +
                         quote_argument(staging.wstring());
    if (run_process(tar, command, true, error)) return true;

    // Older Windows installations may not have tar in System32, but may have
    // it available through PATH (for example via Git for Windows).
    error.clear();
    return run_process({}, L"tar.exe -xf " + quote_argument(archive.wstring()) +
                                L" -C " + quote_argument(staging.wstring()),
                       true, error);
}

bool copy_tree(const std::filesystem::path& source,
               const std::filesystem::path& destination,
               std::wstring& error) {
    std::error_code status_error;
    std::filesystem::create_directories(destination, status_error);
    if (status_error) {
        error = L"Could not create the installation directory.";
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(source,
                                                                   status_error)) {
        if (status_error) break;
        // The helper is the process currently doing the copy, so Windows
        // keeps its image file locked. It is already present in the install
        // directory; leave it in place while the rest of the package updates.
        if (entry.path().filename() == L"gbb-updater.exe") continue;
        const auto target = destination / entry.path().filename();
        std::filesystem::copy(entry.path(), target,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              status_error);
        if (status_error) break;
    }
    if (status_error) {
        error = L"Could not copy the new Go Bigger Boy files.";
        return false;
    }
    return true;
}

int fail(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), L"Go Bigger Boy update",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return 1;
}

} // namespace

int update_main(const int argc, wchar_t** argv) {
    if (argc != 11 || std::wstring_view(argv[1]) != L"--archive" ||
        std::wstring_view(argv[3]) != L"--root" ||
        std::wstring_view(argv[5]) != L"--executable" ||
        std::wstring_view(argv[7]) != L"--sha256" ||
        std::wstring_view(argv[9]) != L"--pid") {
        return fail(L"The update helper received invalid arguments.");
    }

    const std::filesystem::path archive{argv[2]};
    const std::filesystem::path root{argv[4]};
    const std::filesystem::path executable{argv[6]};
    const std::wstring expected_hash{argv[8]};
    wchar_t* end = nullptr;
    const auto process_id = wcstoul(argv[10], &end, 10);
    if (end == argv[10] || *end != L'\0' || process_id == 0) {
        return fail(L"The update helper received an invalid process ID.");
    }

    std::wstring error;
    if (!wait_for_parent(static_cast<DWORD>(process_id), error)) {
        return fail(error);
    }
    std::wstring actual_hash;
    if (!sha256_file(archive, actual_hash, error) ||
        !equals_case_insensitive(actual_hash, expected_hash)) {
        return fail(error.empty()
                        ? L"The downloaded update failed SHA-256 verification."
                        : error);
    }

    const auto staging = archive.parent_path() / L"extracted";
    if (!extract_archive(archive, staging, error)) return fail(error);

    const auto settings = root / L"settings.ini";
    const auto settings_backup = archive.parent_path() / L"settings.ini.user";
    std::error_code status_error;
    const bool had_settings = std::filesystem::exists(settings, status_error);
    if (status_error ||
        (had_settings && !std::filesystem::copy_file(
            settings, settings_backup,
            std::filesystem::copy_options::overwrite_existing, status_error))) {
        return fail(L"Could not preserve the existing settings file.");
    }
    if (!copy_tree(staging, root, error)) return fail(error);
    if (had_settings && !std::filesystem::copy_file(
            settings_backup, settings,
            std::filesystem::copy_options::overwrite_existing, status_error)) {
        return fail(L"Could not restore the existing settings file.");
    }

    std::filesystem::remove_all(staging, status_error);
    std::filesystem::remove(archive, status_error);
    std::filesystem::remove(settings_backup, status_error);

    const auto executable_string = executable.wstring();
    const auto command_string = quote_argument(executable_string);
    std::vector<wchar_t> command(command_string.begin(), command_string.end());
    command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, root.c_str(), &startup, &process)) {
        return fail(L"The update was installed, but Go Bigger Boy could not be restarted.");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return 0;
}

int main() {
    int argc = 0;
    auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) return 1;
    const auto result = update_main(argc, argv);
    LocalFree(argv);
    return result;
}

#endif // _WIN32
