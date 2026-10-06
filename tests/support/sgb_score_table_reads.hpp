// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <ostream>

namespace sgb_test {
// Address-only black-box observation of the first three candidate song words.
// A base write count distinguishes reads before/after table replacement without
// retaining any data values or claiming that an upload completed successfully.
class ScoreTableReads {
public:
    static constexpr unsigned capacity = 256;
    void record(char kind, std::uint64_t master, std::uint64_t half,
                std::uint16_t address, std::uint64_t audible_packets) noexcept {
        if (address < 0x2b00 || address >= 0x2b06) return;
        if (kind == 'W') {
            ++writes_;
            if (address == 0x2b00) ++base_writes_;
        } else if (kind == 'R') {
            ++reads_;
            if (size_ < capacity)
                events_[size_++] = {master, half, audible_packets, base_writes_, address};
        }
    }
    void write_json(std::ostream& out) const {
        out << "{\"schema\":\"gbb-sgb-score-table-reads-v1\",\"qualification\":false,"
               "\"evidence\":\"SPC_bus_addresses\",\"base\":11008,\"bytes\":6,"
               "\"capacity\":256,\"reads\":" << reads_ << ",\"writes\":" << writes_
            << ",\"base_writes\":" << base_writes_ << ",\"overflow\":"
            << (reads_ > capacity ? "true" : "false") << ",\"events\":[";
        for (unsigned i = 0; i < size_; ++i) {
            if (i) out << ',';
            const auto& e = events_[i];
            out << "{\"master_clock\":" << e.master << ",\"spc_half_clock\":" << e.half
                << ",\"address\":" << e.address << ",\"audible_packets\":" << e.audible
                << ",\"base_writes\":" << e.base_writes << '}';
        }
        out << "]}\n";
    }
private:
    struct Event {
        std::uint64_t master{}, half{}, audible{}, base_writes{};
        std::uint16_t address{};
    };
    std::array<Event, capacity> events_{};
    unsigned size_{};
    std::uint64_t reads_{}, writes_{}, base_writes_{};
};
} // namespace sgb_test
