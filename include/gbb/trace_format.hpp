#pragma once

#include <atomic>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace gbb {

inline constexpr std::uint32_t trace_format_version = 1;
inline constexpr std::uint64_t trace_flush_interval_frames = 30;

// A process-wide ID lets traces from two frontend components be correlated
// even when they use different field payloads (for example SDL guest fields
// versus the link-harness automation fields).
inline std::uint64_t next_trace_session_id() noexcept {
    static std::atomic<std::uint64_t> next{0};
    return ++next;
}

inline void write_trace_session_start(
    std::ostream& output, const std::uint64_t session,
    const std::string_view transport, const std::string_view role = {},
    const std::string_view scenario = {}) {
    // Keep the historical `id` field/order for existing trace consumers while
    // exposing the canonical `session_id` alias at the end of the record.
    output << "session_start id=" << session
           << " trace_version=" << trace_format_version
           << " counters_reset=1 transport=" << transport;
    if (!role.empty()) output << " role=" << role;
    if (!scenario.empty()) output << " scenario=" << scenario;
    output << " clock=monotonic elapsed_ms=0 session_id=" << session << '\n';
}

inline void write_trace_session_end(std::ostream& output,
                                    const std::uint64_t session,
                                    const std::uint64_t frames,
                                    const std::uint64_t elapsed_ms) {
    output << "session_end id=" << session << " frames=" << frames
           << " elapsed_ms=" << elapsed_ms << " session_id=" << session
           << '\n';
}

// Every event record uses the same leading fields so SDL traces and harness
// traces can be parsed by one tool. Payload fields remain frontend-specific.
inline void write_trace_event_prefix(
    std::ostream& output, const std::string_view event,
    const std::uint64_t session, const std::uint64_t frame,
    const std::uint64_t elapsed_ms, const std::string_view transport,
    const std::string_view role) {
    output << "event=" << event << " trace_version=" << trace_format_version
           << " session_id=" << session
           << " frame=" << frame << " elapsed_ms=" << elapsed_ms
           << " transport=" << transport << " role=" << role;
}

} // namespace gbb
