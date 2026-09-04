#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <string_view>
#include <string>
#include <chrono>

namespace gbb::link_harness {

// Owns the on-disk lifecycle of a scenario trace.  Record formatting remains
// in the harness so this class can be reused by future transports/scenarios
// without knowing anything about Pokémon WRAM fields.
class ScenarioTraceWriter {
  public:
    ScenarioTraceWriter(const std::filesystem::path& path,
                        std::string_view transport,
                        std::string_view scenario);
    ~ScenarioTraceWriter();

    ScenarioTraceWriter(const ScenarioTraceWriter&) = delete;
    ScenarioTraceWriter& operator=(const ScenarioTraceWriter&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return output_.is_open(); }
    [[nodiscard]] std::ostream& stream() noexcept { return output_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] std::string_view transport() const noexcept { return transport_; }
    [[nodiscard]] std::string_view role() const noexcept { return "harness"; }
    [[nodiscard]] std::uint64_t session() const noexcept { return session_; }
    [[nodiscard]] std::uint64_t elapsed_ms() const noexcept;

    // Marks a frame as the most recent one and periodically checkpoints the
    // buffered stream.  Flushing every frame can perturb serial timing.
    void checkpoint_frame(std::uint64_t frame);
    void flush();
    void finish();

  private:
    std::ofstream output_;
    std::filesystem::path path_;
    std::uint64_t frame_{};
    std::uint64_t session_{};
    std::chrono::steady_clock::time_point started_at_{};
    std::string transport_;
    bool finished_{};
};

} // namespace gbb::link_harness
