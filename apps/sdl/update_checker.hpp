#pragma once

#include <mutex>
#include <optional>
#include <filesystem>
#include <string>
#include <thread>

namespace gbb_desktop {

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
                                   std::string& error);

private:
    std::mutex mutex_;
    std::thread worker_;
    std::optional<UpdateInfo> update_;
    std::string error_;
    bool complete_{};
    bool consumed_{};
};

class UpdateDownload {
public:
    UpdateDownload(UpdateInfo release, std::filesystem::path directory);
    ~UpdateDownload();
    UpdateDownload(const UpdateDownload&) = delete;
    UpdateDownload& operator=(const UpdateDownload&) = delete;
    [[nodiscard]] bool take_result(std::optional<DownloadedUpdate>& update,
                                   std::string& error);
private:
    std::mutex mutex_;
    std::thread worker_;
    std::optional<DownloadedUpdate> update_;
    std::string error_;
    bool complete_{};
    bool consumed_{};
};

[[nodiscard]] bool launch_update_installer(
    const DownloadedUpdate& update,
    const std::filesystem::path& installation_root,
    const std::filesystem::path& executable,
    std::string& error);

} // namespace gbb_desktop
