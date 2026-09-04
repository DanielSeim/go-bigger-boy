#include "gbb/trace_parser.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>

namespace gbb {
namespace {

std::optional<std::uint64_t> parse_uint(std::string_view value) noexcept {
    if (value.empty()) return std::nullopt;
    int base = 10;
    if (value.size() > 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value.remove_prefix(2);
    }
    if (value.empty()) return std::nullopt;
    std::uint64_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                        parsed, base);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::string_view first_token(std::string_view line) noexcept {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
        line.remove_prefix(1);
    }
    const auto end = line.find_first_of(" \t");
    return line.substr(0, end == std::string_view::npos ? line.size() : end);
}

bool starts_with(const std::string_view value, const std::string_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool parse_fields(std::string_view line, TraceRecord& record) {
    std::size_t offset{};
    while (offset < line.size()) {
        while (offset < line.size() &&
               std::isspace(static_cast<unsigned char>(line[offset]))) {
            ++offset;
        }
        if (offset >= line.size() || line[offset] == '#') break;
        const auto token_start = offset;
        while (offset < line.size() &&
               !std::isspace(static_cast<unsigned char>(line[offset]))) {
            ++offset;
        }
        const auto token = line.substr(token_start, offset - token_start);
        const auto equals = token.find('=');
        if (equals == std::string_view::npos || equals == 0) continue;
        const auto key = std::string{token.substr(0, equals)};
        if (record.fields.find(key) == record.fields.end() &&
            record.fields.size() >= trace_parser_max_fields_per_record) {
            return false;
        }
        record.fields[key] =
            std::string{token.substr(equals + 1)};
    }
    return true;
}

bool has_field(const TraceRecord& record, const std::string_view name) {
    return record.fields.find(std::string{name}) != record.fields.end();
}

} // namespace

const std::string* TraceRecord::field(const std::string_view name) const noexcept {
    const auto found = fields.find(std::string{name});
    return found == fields.end() ? nullptr : &found->second;
}

std::optional<std::uint64_t> TraceRecord::uint64_field(
    const std::string_view name) const noexcept {
    const auto* value = field(name);
    return value == nullptr ? std::nullopt : parse_uint(*value);
}

bool TraceReport::has_event(const std::string_view name) const noexcept {
    for (const auto& record : records) {
        if (record.kind == TraceRecordKind::event && record.event == name) {
            return true;
        }
    }
    return false;
}

TraceReport parse_trace(const std::string_view text) {
    TraceReport report;
    if (text.size() > trace_parser_max_bytes) {
        report.errors.push_back("trace exceeds the 16 MiB parser limit");
    }
    auto input = text.substr(0, std::min(text.size(), trace_parser_max_bytes));
    std::size_t offset{};
    std::size_t line_number{};
    auto add_error = [&](const std::string_view message) {
        if (report.errors.size() < 256U) {
            report.errors.push_back("line " + std::to_string(line_number) + ": " +
                                    std::string{message});
        }
    };
    auto update_context = [&](const TraceRecord& record) {
        const auto version = record.uint64_field("trace_version");
        if (version.has_value()) {
            if (*version > std::numeric_limits<std::uint32_t>::max()) {
                add_error("trace_version is out of range");
            } else if (report.trace_version == 0) {
                report.trace_version = static_cast<std::uint32_t>(*version);
            } else if (report.trace_version != *version) {
                add_error("trace_version changed within one trace");
            }
        }
        const auto session = record.uint64_field("session_id");
        if (session.has_value()) {
            if (!report.session_id.has_value()) {
                report.session_id = *session;
            } else if (*report.session_id != *session) {
                add_error("session_id changed within one trace");
            }
        }
        if (const auto* transport = record.field("transport"); transport != nullptr) {
            if (report.transport.empty()) report.transport = *transport;
            else if (report.transport != *transport)
                add_error("transport changed within one trace");
        }
        if (const auto* role = record.field("role"); role != nullptr) {
            if (report.role.empty()) report.role = *role;
            else if (report.role != *role) add_error("role changed within one trace");
        }
    };
    auto validate_canonical_event = [&](const TraceRecord& record) {
        static constexpr std::string_view required[] = {
            "trace_version", "session_id", "frame", "elapsed_ms", "transport",
            "role"};
        for (const auto name : required) {
            if (!has_field(record, name)) {
                add_error("canonical event is missing " + std::string{name});
            }
        }
        for (const auto name : {std::string_view{"trace_version"},
                                std::string_view{"session_id"},
                                std::string_view{"frame"},
                                std::string_view{"elapsed_ms"}}) {
            if (has_field(record, name) && !record.uint64_field(name).has_value()) {
                add_error(std::string{name} + " is not an unsigned integer");
            }
        }
        update_context(record);
    };
    auto update_frame = [&](const TraceRecord& record, const bool canonical) {
        const auto frame = record.uint64_field("frame");
        if (!frame.has_value()) return;
        if (!report.has_frame) {
            report.first_frame = *frame;
            report.has_frame = true;
        } else if (*frame < report.last_frame) {
            report.frames_monotonic = false;
            add_error(canonical ? "frame values are not monotonic"
                                : "legacy frame values are not monotonic");
        }
        report.last_frame = *frame;
    };

    while (offset <= input.size()) {
        const auto line_end = input.find('\n', offset);
        auto line = input.substr(offset, line_end == std::string_view::npos
                                         ? input.size() - offset
                                         : line_end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ++line_number;
        if (line.size() > trace_parser_max_line_bytes) {
            add_error("trace record exceeds the 1 MiB line limit");
            if (line_end == std::string_view::npos) break;
            offset = line_end + 1;
            continue;
        }
        const auto token = first_token(line);
        if (!token.empty() && token.front() != '#') {
            TraceRecord record;
            record.line = line_number;
            if (token == "session_start") {
                record.kind = TraceRecordKind::session_start;
                if (!parse_fields(line.substr(line.find(token) + token.size()), record))
                    add_error("session_start has too many fields");
                if (report.session_started) add_error("duplicate session_start");
                report.session_started = true;
                if (!has_field(record, "trace_version") ||
                    !record.uint64_field("trace_version").has_value()) {
                    add_error("session_start has no valid trace_version");
                }
                const auto id = record.uint64_field("session_id");
                const auto legacy_id = record.uint64_field("id");
                if (!id.has_value() && !legacy_id.has_value()) {
                    add_error("session_start has no session id");
                } else if (id.has_value() && legacy_id.has_value() && *id != *legacy_id) {
                    add_error("session_start id and session_id disagree");
                }
                update_context(record);
                if (!id.has_value() && legacy_id.has_value())
                    report.session_id = *legacy_id;
                if (const auto* scenario = record.field("scenario"); scenario != nullptr)
                    report.scenario = *scenario;
            } else if (token == "session_end") {
                record.kind = TraceRecordKind::session_end;
                if (!parse_fields(line.substr(line.find(token) + token.size()), record))
                    add_error("session_end has too many fields");
                if (report.session_ended) add_error("duplicate session_end");
                report.session_ended = true;
                const auto id = record.uint64_field("session_id");
                const auto legacy_id = record.uint64_field("id");
                if (id.has_value() && legacy_id.has_value() && *id != *legacy_id)
                    add_error("session_end id and session_id disagree");
                update_context(record);
                if (!id.has_value() && legacy_id.has_value()) {
                    if (report.session_id.has_value() &&
                        *report.session_id != *legacy_id)
                        add_error("session_end id disagrees with session_start");
                    else if (!report.session_id.has_value())
                        report.session_id = *legacy_id;
                }
                for (const auto name : {std::string_view{"frames"},
                                        std::string_view{"elapsed_ms"}}) {
                    if (!has_field(record, name) || !record.uint64_field(name).has_value())
                        add_error("session_end has no valid " + std::string{name});
                }
            } else if (starts_with(token, "event=")) {
                record.kind = TraceRecordKind::event;
                record.canonical = true;
                if (!parse_fields(line, record))
                    add_error("canonical event has too many fields");
                if (const auto* event = record.field("event"); event != nullptr)
                    record.event = *event;
                if (record.event.empty()) add_error("canonical event has no event name");
                validate_canonical_event(record);
                ++report.canonical_events;
                update_frame(record, true);
                if (record.event == "serial_complete") ++report.serial_completions;
                if (record.event == "serial_active") ++report.serial_active_changes;
                if (record.event == "pokemon_state") ++report.pokemon_state_events;
                if (record.event == "trade_input_phase") ++report.trade_phase_events;
                if (record.event.find("stall") != std::string::npos) ++report.stall_events;
            } else if (starts_with(token, "frame=")) {
                record.kind = TraceRecordKind::legacy_frame;
                if (!parse_fields(line, record))
                    add_error("legacy frame has too many fields");
                record.event = "frame";
                update_frame(record, false);
            } else if (token == "trace_end") {
                record.kind = TraceRecordKind::trace_end;
                if (!parse_fields(line.substr(line.find(token) + token.size()), record))
                    add_error("trace_end has too many fields");
            } else if (token.find('=') != std::string_view::npos) {
                record.kind = TraceRecordKind::metadata;
                if (!parse_fields(line, record))
                    add_error("metadata record has too many fields");
                if (const auto* transport = record.field("transport"); transport != nullptr &&
                    report.transport.empty()) report.transport = *transport;
                if (const auto* scenario = record.field("scenario"); scenario != nullptr &&
                    report.scenario.empty()) report.scenario = *scenario;
            }
            if (record.kind != TraceRecordKind::unknown) {
                if (report.records.size() >= trace_parser_max_records) {
                    add_error("trace contains too many records");
                    break;
                }
                report.records.push_back(std::move(record));
            }
        }
        if (line_end == std::string_view::npos) break;
        offset = line_end + 1;
    }
    return report;
}

} // namespace gbb
