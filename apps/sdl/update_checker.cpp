#include "update_checker.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#endif

namespace gbb_desktop {
namespace {
constexpr std::string_view latest_release_url =
    "https://github.com/DanielSeim/go-bigger-boy/releases/latest";
constexpr std::size_t maximum_response_size = 256 * 1024;

std::string platform_asset_name() {
#ifdef _WIN32
    return "go-bigger-boy-windows-x64.zip";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "go-bigger-boy-macos-arm64.tar.gz";
#elif defined(__APPLE__)
    return "go-bigger-boy-macos-x64.tar.gz";
#elif defined(__x86_64__)
    return "go-bigger-boy-linux-x64.tar.gz";
#else
    return {};
#endif
}

std::optional<std::array<unsigned, 3>> parse_version(std::string_view text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
        text.remove_prefix(1);
    }
    std::array<unsigned, 3> version{};
    for (std::size_t part = 0; part < version.size(); ++part) {
        const auto* begin = text.data();
        const auto* end = begin + text.size();
        const auto parsed = std::from_chars(begin, end, version[part]);
        if (parsed.ec != std::errc{} || parsed.ptr == begin) return std::nullopt;
        const auto consumed = static_cast<std::size_t>(parsed.ptr - begin);
        text.remove_prefix(consumed);
        if (part + 1 < version.size()) {
            if (text.empty() || text.front() != '.') return std::nullopt;
            text.remove_prefix(1);
        }
    }
    if (!text.empty() && text.front() != '-' && text.front() != '+') {
        return std::nullopt;
    }
    return version;
}

std::optional<std::string> json_string_field(const std::string& json,
                                             const std::string_view field) {
    const auto key = '"' + std::string(field) + '"';
    auto position = json.find(key);
    if (position == std::string::npos) return std::nullopt;
    position = json.find(':', position + key.size());
    if (position == std::string::npos) return std::nullopt;
    position = json.find('"', position + 1);
    if (position == std::string::npos) return std::nullopt;
    const auto end = json.find('"', position + 1);
    if (end == std::string::npos) return std::nullopt;
    return json.substr(position + 1, end - position - 1);
}

std::optional<std::string> asset_string_field(const std::string& json,
                                              const std::string& asset,
                                              const std::string_view field) {
    const auto marker = "\"name\":\"" + asset + "\"";
    const auto asset_position = json.find(marker);
    if (asset_position == std::string::npos) return std::nullopt;
    const auto next_asset = json.find("\"name\":\"", asset_position + marker.size());
    const auto key = '"' + std::string(field) + '"';
    auto position = json.find(key, asset_position);
    if (position == std::string::npos ||
        (next_asset != std::string::npos && position >= next_asset)) {
        return std::nullopt;
    }
    position = json.find(':', position + key.size());
    position = position == std::string::npos ? position : json.find('"', position + 1);
    if (position == std::string::npos) return std::nullopt;
    const auto end = json.find('"', position + 1);
    if (end == std::string::npos) return std::nullopt;
    return json.substr(position + 1, end - position - 1);
}

#ifndef _WIN32
std::string shell_quote(const std::string& value) {
    std::string quoted{"'"};
    for (const auto character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted += character;
    }
    return quoted + '\'';
}
#endif

#ifdef _WIN32
std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), wide.data(), size);
    return wide;
}

std::wstring quote_windows_argument(const std::wstring& value) {
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

std::optional<std::string> sha256_file(
    const std::filesystem::path& path, std::string& error) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0) {
        error = "could not initialize SHA-256 verification";
        return std::nullopt;
    }
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD returned = 0;
    const bool properties_ok =
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size),
                          sizeof(object_size), &returned, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_size),
                          sizeof(hash_size), &returned, 0) == 0;
    if (!properties_ok) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "could not initialize SHA-256 verification";
        return std::nullopt;
    }
    std::vector<UCHAR> object(object_size);
    std::vector<UCHAR> hash(hash_size);
    BCRYPT_HASH_HANDLE state{};
    if (BCryptCreateHash(algorithm, &state, object.data(), object_size,
                         nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        error = "could not initialize SHA-256 verification";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    bool success = static_cast<bool>(input);
    while (success && input.read(buffer.data(), buffer.size())) {
        success = BCryptHashData(
                      state, reinterpret_cast<PUCHAR>(buffer.data()),
                      static_cast<ULONG>(input.gcount()), 0) == 0;
    }
    if (success && input.gcount() > 0) {
        success = BCryptHashData(
                      state, reinterpret_cast<PUCHAR>(buffer.data()),
                      static_cast<ULONG>(input.gcount()), 0) == 0;
    }
    if (success) {
        success = BCryptFinishHash(state, hash.data(), hash_size, 0) == 0;
    }
    BCryptDestroyHash(state);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) {
        error = "could not read or verify the downloaded release";
        return std::nullopt;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string digest;
    digest.reserve(hash.size() * 2);
    for (const auto byte : hash) {
        digest += digits[byte >> 4];
        digest += digits[byte & 0x0f];
    }
    return digest;
}
#endif

bool download_asset(const UpdateInfo& release,
                    const std::filesystem::path& archive,
                    std::string& error, DownloadProgress* progress) {
    return download_public_file(release.asset_url, archive,
                                512 * 1024 * 1024, error, progress);
}

bool verify_asset(const UpdateInfo& release,
                  const std::filesystem::path& archive,
                  std::string& error) {
#ifdef _WIN32
    const auto digest = sha256_file(archive, error);
    return digest && *digest == release.sha256;
#elif defined(__APPLE__)
    const auto command = "test \"$(shasum -a 256 " +
        shell_quote(archive.u8string()) + " | cut -d ' ' -f 1)\" = " +
        shell_quote(release.sha256);
#else
    const auto command = "test \"$(sha256sum " +
        shell_quote(archive.u8string()) + " | cut -d ' ' -f 1)\" = " +
        shell_quote(release.sha256);
#endif
#ifndef _WIN32
    if (std::system(command.c_str()) != 0) {
        error = "downloaded update failed SHA-256 verification";
        return false;
    }
    return true;
#endif
}

#ifdef _WIN32
class InternetHandle {
public:
    explicit InternetHandle(HINTERNET value = nullptr) : value_(value) {}
    ~InternetHandle() {
        if (value_ != nullptr) WinHttpCloseHandle(value_);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

private:
    HINTERNET value_{};
};

std::string fetch_latest_release(std::string& error) {
    InternetHandle session{WinHttpOpen(
        L"Go-Bigger-Boy/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) {
        error = "could not initialize Windows HTTP";
        return {};
    }
    static_cast<void>(WinHttpSetTimeouts(session.get(), 3000, 3000, 5000,
                                         5000));
    InternetHandle connection{
        WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection) {
        error = "could not connect to GitHub";
        return {};
    }
    InternetHandle request{WinHttpOpenRequest(
        connection.get(), L"GET",
        L"/repos/DanielSeim/go-bigger-boy/releases/latest", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request) {
        error = "could not create the GitHub update request";
        return {};
    }
    constexpr auto headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1L),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "GitHub update request failed";
        return {};
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        error = "GitHub returned HTTP status " + std::to_string(status);
        return {};
    }

    std::string response;
    while (response.size() < maximum_response_size) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            error = "could not read the GitHub update response";
            return {};
        }
        if (available == 0) return response;
        const auto remaining = maximum_response_size - response.size();
        const auto requested = static_cast<DWORD>(
            std::min<std::size_t>(available, remaining));
        const auto offset = response.size();
        response.resize(offset + requested);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.data() + offset,
                             requested, &read)) {
            error = "could not read the GitHub update response";
            return {};
        }
        response.resize(offset + read);
    }
    error = "GitHub update response was unexpectedly large";
    return {};
}

bool download_windows_file(const std::string& url,
                           const std::filesystem::path& destination,
                           const std::uintmax_t maximum_size,
                           std::string& error, DownloadProgress* progress) {
    const auto wide_url = widen(url);
    std::vector<wchar_t> host(256);
    std::vector<wchar_t> path(4096);
    std::vector<wchar_t> extra(4096);
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = path.data();
    components.dwUrlPathLength = static_cast<DWORD>(path.size());
    components.lpszExtraInfo = extra.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extra.size());
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
        error = "could not parse the update download URL";
        return false;
    }
    InternetHandle session{WinHttpOpen(
        L"Go-Bigger-Boy/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) {
        error = "could not initialize Windows HTTP";
        return false;
    }
    static_cast<void>(WinHttpSetTimeouts(session.get(), 5000, 5000, 15000,
                                         15000));
    InternetHandle connection{WinHttpConnect(
        session.get(), components.lpszHostName, components.nPort, 0)};
    if (!connection) {
        error = "could not connect to the update server";
        return false;
    }
    std::wstring request_path{components.lpszUrlPath,
                              components.dwUrlPathLength};
    request_path.append(components.lpszExtraInfo,
                        components.dwExtraInfoLength);
    const auto flags = components.nScheme == INTERNET_SCHEME_HTTPS
                           ? WINHTTP_FLAG_SECURE
                           : 0;
    InternetHandle request{WinHttpOpenRequest(
        connection.get(), L"GET", request_path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!request ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "update download failed";
        return false;
    }

    if (progress != nullptr) {
        DWORD content_length = 0;
        DWORD content_length_size = sizeof(content_length);
        if (WinHttpQueryHeaders(
                request.get(),
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &content_length,
                &content_length_size, WINHTTP_NO_HEADER_INDEX)) {
            progress->total_bytes.store(content_length);
        }
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &status_size, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        error = "update server returned HTTP status " + std::to_string(status);
        return false;
    }

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not create the update download file";
        return false;
    }
    std::array<char, 64 * 1024> buffer{};
    std::uintmax_t total = 0;
    while (true) {
        if (progress != nullptr &&
            progress->cancel_requested.load(std::memory_order_relaxed)) {
            error = "download cancelled";
            return false;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            error = "could not read the update download";
            return false;
        }
        if (available == 0) break;
        while (available > 0) {
            const auto requested = static_cast<DWORD>(std::min<std::size_t>(
                available, buffer.size()));
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), buffer.data(), requested,
                                 &read) || read == 0) {
                error = "could not read the update download";
                return false;
            }
            total += read;
            if (progress != nullptr) {
                progress->completed_bytes.store(total,
                                                std::memory_order_relaxed);
            }
            if (total > maximum_size) {
                error = "downloaded file is too large";
                return false;
            }
            output.write(buffer.data(), read);
            if (!output) {
                error = "could not store the update download";
                return false;
            }
            available -= read;
        }
    }
    output.close();
    if (total == 0) {
        error = "the update download was empty";
        return false;
    }
    return true;
}
#else
std::string fetch_latest_release(std::string& error) {
    constexpr auto command =
        "curl -fsSL --max-time 5 -A 'Go-Bigger-Boy/1.0' "
        "-H 'Accept: application/vnd.github+json' "
        "-H 'X-GitHub-Api-Version: 2022-11-28' "
        "https://api.github.com/repos/DanielSeim/go-bigger-boy/releases/latest "
        "2>/dev/null";
    FILE* pipe = popen(command, "r");
    if (pipe == nullptr) {
        error = "could not start the system HTTP client";
        return {};
    }
    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < maximum_response_size) {
        const auto count = std::fread(buffer.data(), 1, buffer.size(), pipe);
        response.append(buffer.data(), count);
        if (count < buffer.size()) break;
    }
    const auto status = pclose(pipe);
    if (status != 0) {
        error = "GitHub update request failed";
        return {};
    }
    if (response.size() >= maximum_response_size) {
        error = "GitHub update response was unexpectedly large";
        return {};
    }
    return response;
}
#endif
} // namespace

bool download_public_file(const std::string& url,
                          const std::filesystem::path& destination,
                          const std::uintmax_t maximum_size,
                          std::string& error, DownloadProgress* progress) {
    if (progress != nullptr) {
        progress->completed_bytes.store(0, std::memory_order_relaxed);
        progress->total_bytes.store(0, std::memory_order_relaxed);
        if (progress->cancel_requested.load(std::memory_order_relaxed)) {
            error = "download cancelled";
            return false;
        }
    }
    std::filesystem::create_directories(destination.parent_path());
    auto temporary = destination;
    temporary += ".download";
#ifdef _WIN32
    if (!download_windows_file(url, temporary, maximum_size, error,
                               progress)) {
        std::filesystem::remove(temporary);
        return false;
    }
#else
    const auto child = fork();
    if (child < 0) {
        error = "could not start the system HTTP client";
        return false;
    }
    if (child == 0) {
        const auto null = open("/dev/null", O_WRONLY);
        if (null >= 0) {
            static_cast<void>(dup2(null, STDOUT_FILENO));
            static_cast<void>(dup2(null, STDERR_FILENO));
            close(null);
        }
        execlp("curl", "curl", "-fL", "--max-time", "20", "-sS", "-o",
               temporary.c_str(), url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (true) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0) {
            static_cast<void>(kill(child, SIGTERM));
            static_cast<void>(waitpid(child, &status, 0));
            error = "could not read the system HTTP client status";
            return false;
        }
        if (progress != nullptr &&
            progress->cancel_requested.load(std::memory_order_relaxed)) {
            static_cast<void>(kill(child, SIGTERM));
            static_cast<void>(waitpid(child, &status, 0));
            error = "download cancelled";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        error = "download failed";
        return false;
    }
#endif
    if (progress != nullptr &&
        progress->cancel_requested.load(std::memory_order_relaxed)) {
        std::filesystem::remove(temporary);
        error = "download cancelled";
        return false;
    }
    std::error_code size_error;
    const auto size = std::filesystem::file_size(temporary, size_error);
    if (size_error || size == 0 || size > maximum_size) {
        std::filesystem::remove(temporary);
        error = "downloaded file has an invalid size";
        return false;
    }
    std::error_code replace_error;
    std::filesystem::remove(destination, replace_error);
    replace_error.clear();
    std::filesystem::rename(temporary, destination, replace_error);
    if (replace_error) {
        std::filesystem::remove(temporary);
        error = "could not store downloaded file";
        return false;
    }
    return true;
}

UpdateChecker::UpdateChecker(std::string current_version) {
    try {
        worker_ = std::thread(
            [this, current_version = std::move(current_version)] {
                std::string error;
                std::optional<UpdateInfo> update;
                try {
                    const auto response = fetch_latest_release(error);
                    if (error.empty()) {
                        const auto tag =
                            json_string_field(response, "tag_name");
                        const auto current = parse_version(current_version);
                        const auto latest =
                            tag ? parse_version(*tag) : std::nullopt;
                        if (!tag || !latest) {
                            error =
                                "GitHub returned an invalid release version";
                        } else if (!current) {
                            error = "this build has an invalid version";
                        } else if (*latest > *current) {
                            const auto asset = platform_asset_name();
                            if (asset.empty()) {
                                error = "automatic updates are unavailable for "
                                        "this platform architecture";
                            }
                            const auto asset_url = asset_string_field(
                                response, asset, "browser_download_url");
                            const auto digest = asset_string_field(
                                response, asset, "digest");
                            if (error.empty() && (!asset_url || !digest ||
                                digest->rfind("sha256:", 0) != 0 ||
                                digest->size() != 71)) {
                                error = "latest release has no verified asset " +
                                        asset;
                            } else if (error.empty()) {
                                update = UpdateInfo{
                                    *tag, std::string(latest_release_url), asset,
                                    *asset_url, digest->substr(7)};
                            }
                        }
                    }
                } catch (const std::exception& exception) {
                    error = std::string("update check failed: ") +
                            exception.what();
                } catch (...) {
                    error = "update check failed unexpectedly";
                }
                const std::lock_guard lock(mutex_);
                update_ = std::move(update);
                error_ = std::move(error);
                complete_ = true;
            });
    } catch (const std::exception& exception) {
        error_ = std::string("could not start update check: ") +
                 exception.what();
        complete_ = true;
    }
}

UpdateChecker::~UpdateChecker() {
    if (worker_.joinable()) worker_.join();
}

bool UpdateChecker::take_result(std::optional<UpdateInfo>& update,
                                std::string& error) {
    const std::lock_guard lock(mutex_);
    if (!complete_ || consumed_) return false;
    update = std::move(update_);
    error = std::move(error_);
    consumed_ = true;
    return true;
}

UpdateDownload::UpdateDownload(UpdateInfo release,
                               std::filesystem::path directory) {
    try {
        worker_ = std::thread(
            [this, release = std::move(release),
             directory = std::move(directory)]() mutable {
                std::string error;
                std::optional<DownloadedUpdate> update;
                try {
                    const auto archive = directory / release.asset_name;
                    if (download_asset(release, archive, error, &progress_) &&
                        !progress_.cancel_requested.load(
                            std::memory_order_relaxed) &&
                        verify_asset(release, archive, error)) {
                        update = DownloadedUpdate{std::move(release), archive};
                    } else {
                        std::error_code ignored;
                        std::filesystem::remove(archive, ignored);
                    }
                } catch (const std::exception& exception) {
                    error = std::string("update download failed: ") +
                            exception.what();
                }
                const std::lock_guard lock(mutex_);
                update_ = std::move(update);
                error_ = std::move(error);
                complete_ = true;
            });
    } catch (const std::exception& exception) {
        error_ = std::string("could not start update download: ") +
                 exception.what();
        complete_ = true;
    }
}

UpdateDownload::~UpdateDownload() {
    if (worker_.joinable()) worker_.join();
}

bool UpdateDownload::take_result(std::optional<DownloadedUpdate>& update,
                                 std::string& error) {
    const std::lock_guard lock(mutex_);
    if (!complete_ || consumed_) return false;
    update = std::move(update_);
    error = std::move(error_);
    consumed_ = true;
    return true;
}

bool launch_update_installer(const DownloadedUpdate& update,
                             const std::filesystem::path& installation_root,
                             const std::filesystem::path& executable,
                             std::string& error) {
    const auto directory = update.archive.parent_path();
    const auto staging = directory / "extracted";
    const auto settings = installation_root / "settings.ini";
    const auto settings_backup = directory / "settings.ini.user";
#ifdef _WIN32
    const auto helper = installation_root / "gbb-updater.exe";
    if (!std::filesystem::exists(helper)) {
        error = "the native Windows update helper is missing";
        return false;
    }
    const auto helper_string = helper.wstring();
    const auto command = quote_windows_argument(helper_string) +
        L" --archive " + quote_windows_argument(widen(update.archive.u8string())) +
        L" --root " + quote_windows_argument(widen(installation_root.u8string())) +
        L" --executable " + quote_windows_argument(widen(executable.u8string())) +
        L" --sha256 " + quote_windows_argument(widen(update.release.sha256)) +
        L" --pid " + std::to_wstring(GetCurrentProcessId());
    auto mutable_command = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(helper_string.c_str(), mutable_command.data(), nullptr,
                        nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        error = "could not start the native Windows update helper";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    const auto script_path = directory / "install-update.sh";
    std::ofstream script(script_path, std::ios::trunc);
    if (!script) {
        error = "could not create the update helper";
        return false;
    }
    script << "#!/bin/sh\nset -e\n"
           << "while kill -0 " << getpid()
           << " 2>/dev/null; do sleep 1; done\n"
#ifdef __APPLE__
           << "test \"$(shasum -a 256 "
#else
           << "test \"$(sha256sum "
#endif
           << shell_quote(update.archive.u8string())
           << " | cut -d ' ' -f 1)\" = "
           << shell_quote(update.release.sha256) << "\n"
           << "rm -rf " << shell_quote(staging.u8string()) << "\n"
           << "mkdir -p " << shell_quote(staging.u8string()) << "\n"
           << "tar -xzf " << shell_quote(update.archive.u8string()) << " -C "
           << shell_quote(staging.u8string()) << "\n"
           << "had_settings=0\nif [ -f " << shell_quote(settings.u8string())
           << " ]; then had_settings=1; cp "
           << shell_quote(settings.u8string()) << " "
           << shell_quote(settings_backup.u8string()) << "; fi\n"
           << "cp -a " << shell_quote((staging / ".").u8string()) << " "
           << shell_quote(installation_root.u8string()) << "\n"
           << "if [ \"$had_settings\" -eq 1 ]; then cp "
           << shell_quote(settings_backup.u8string()) << " "
           << shell_quote(settings.u8string()) << "; else rm -f "
           << shell_quote(settings.u8string()) << "; fi\n"
           << "exec " << shell_quote(executable.u8string()) << "\n";
    script.close();
    const auto child = fork();
    if (child < 0) {
        error = "could not start the update helper";
        return false;
    }
    if (child == 0) {
        execl("/bin/sh", "sh", script_path.c_str(), nullptr);
        _exit(127);
    }
    return true;
#endif
}

} // namespace gbb_desktop
