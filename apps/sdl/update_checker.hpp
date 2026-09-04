#pragma once

#include "gbb/log.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <filesystem>
#include <string>
#include <thread>

namespace gbb_desktop {

// Shared progress/cancellation state for background HTTP downloads. The
// worker updates these atomics while the UI polls them from the main thread.
struct DownloadProgress {
    std::atomic<std::uintmax_t> completed_bytes{};
    std::atomic<std::uintmax_t> total_bytes{};
    std::atomic_bool cancel_requested{};
};

struct UpdateInfo {
    std::string version;
    std::string url;
    std::string asset_name;
    std::string asset_url;
    std::string sha256;
};

struct DownloadedUpdate {
    UpdateInfo release;
    std::filesystem::path archive;
};

class UpdateChecker {
public:
    explicit UpdateChecker(std::string current_version);
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    [[nodiscard]] bool take_result(std::optional<UpdateInfo>& update,
                                   std::string& error,
                                   gbb::LogContext* diagnostic_context = nullptr);

private:
    std::mutex mutex_;
    std::thread worker_;
    std::optional<UpdateInfo> update_;
    std::string error_;
    bool complete_{};
    bool consumed_{};
    gbb::LogContext diagnostic_context_{};
};

class UpdateDownload {
public:
    UpdateDownload(UpdateInfo release, std::filesystem::path directory);
    ~UpdateDownload();
    UpdateDownload(const UpdateDownload&) = delete;
    UpdateDownload& operator=(const UpdateDownload&) = delete;
    void cancel() noexcept { progress_.cancel_requested.store(true); }
    [[nodiscard]] bool cancelled() const noexcept {
        return progress_.cancel_requested.load();
    }
    [[nodiscard]] std::uintmax_t downloaded_bytes() const noexcept {
        return progress_.completed_bytes.load();
    }
    [[nodiscard]] std::uintmax_t total_bytes() const noexcept {
        return progress_.total_bytes.load();
    }
    [[nodiscard]] bool take_result(std::optional<DownloadedUpdate>& update,
                                   std::string& error,
                                   gbb::LogContext* diagnostic_context = nullptr);
private:
    std::mutex mutex_;
    std::thread worker_;
    std::optional<DownloadedUpdate> update_;
    std::string error_;
    bool complete_{};
    bool consumed_{};
    gbb::LogContext diagnostic_context_{};
    DownloadProgress progress_;
};

[[nodiscard]] bool launch_update_installer(
    const DownloadedUpdate& update,
    const std::filesystem::path& installation_root,
    const std::filesystem::path& executable,
    std::string& error);

[[nodiscard]] bool download_public_file(
    const std::string& url, const std::filesystem::path& destination,
    std::uintmax_t maximum_size, std::string& error,
    DownloadProgress* progress = nullptr);

} // namespace gbb_desktop
