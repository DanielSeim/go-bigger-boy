#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gbb {

inline constexpr std::size_t trace_parser_max_bytes = 16U * 1024U * 1024U;
inline constexpr std::size_t trace_parser_max_line_bytes = 1024U * 1024U;
inline constexpr std::size_t trace_parser_max_records = 100000U;
inline constexpr std::size_t trace_parser_max_fields_per_record = 512U;

enum class TraceRecordKind {
    unknown,
    metadata,
    session_start,
    session_end,
    event,
    legacy_frame,
    trace_end,
};

struct TraceRecord {
    std::size_t line{};
    TraceRecordKind kind{TraceRecordKind::unknown};
    bool canonical{};
    std::string event;
    std::unordered_map<std::string, std::string> fields;

    [[nodiscard]] const std::string* field(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> uint64_field(
        std::string_view name) const noexcept;
};

// A parsed trace is intentionally a lightweight report rather than a replay
// of emulator state. It validates the stable envelope and summarizes event
// classes so callers can build deterministic replay/assertion tooling without
// depending on SDL or a particular emulator core.
struct TraceReport {
    std::vector<TraceRecord> records;
    std::vector<std::string> errors;

    std::uint32_t trace_version{};
    std::optional<std::uint64_t> session_id;
    std::string transport;
    std::string role;
    std::string scenario;
    bool session_started{};
    bool session_ended{};
    bool frames_monotonic{true};
    bool has_frame{};
    std::uint64_t first_frame{};
    std::uint64_t last_frame{};
    std::size_t canonical_events{};
    std::size_t serial_completions{};
    std::size_t serial_active_changes{};
    std::size_t pokemon_state_events{};
    std::size_t trade_phase_events{};
    std::size_t stall_events{};

    [[nodiscard]] bool valid() const noexcept { return errors.empty(); }
    [[nodiscard]] bool has_event(std::string_view name) const noexcept;
};

[[nodiscard]] TraceReport parse_trace(std::string_view text);

} // namespace gbb
