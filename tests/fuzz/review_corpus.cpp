#include "gameboy/link_transport.hpp"
#include "gameboy/ppu.hpp"
#include "gameboy/save_state_error.hpp"
#include "gbb/settings.hpp"
#include "gbb/trace_parser.hpp"

#include "save_state_container.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t sgb_packet_size = 16U * 7U;
constexpr std::size_t settings_max_bytes = 2U * 1024U * 1024U;

struct Candidate {
    std::filesystem::path path;
    std::vector<std::uint8_t> bytes;
};

bool read_file(const std::filesystem::path& path,
               std::vector<std::uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    return input.good() || input.eof();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: gameboy_parser_review CANDIDATE_CORPUS\n";
        return 2;
    }
    const std::filesystem::path corpus = argv[1];
    std::error_code error;
    if (!std::filesystem::is_directory(corpus, error) || error) {
        std::cerr << "candidate corpus directory not found: " << corpus << '\n';
        return 2;
    }

    std::vector<Candidate> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(corpus, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || error) continue;
        const auto name = entry.path().filename().string();
        if (name.find('\t') != std::string::npos ||
            name.find('\n') != std::string::npos ||
            name.find('\r') != std::string::npos) {
            std::cerr << "candidate filename contains a tab or newline: " << name
                      << '\n';
            return 1;
        }
        Candidate candidate{entry.path(), {}};
        if (!read_file(candidate.path, candidate.bytes)) {
            std::cerr << "unable to read candidate: " << candidate.path << '\n';
            return 1;
        }
        candidates.push_back(std::move(candidate));
    }
    if (error) {
        std::cerr << "unable to enumerate candidate corpus: " << corpus << '\n';
        return 1;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
                  return left.path.filename() < right.path.filename();
              });
    if (candidates.empty()) {
        std::cerr << "candidate corpus contains no regular files: " << corpus
                  << '\n';
        return 1;
    }

    std::cout << "# gbb semantic corpus review v1\n";
    std::cout << "# file\\tbytes\\tsettings_entries\\ttrace_valid\\ttrace_records\\t"
                 "trace_errors\\tlink_packet\\tsave_state\\tsgb_command\n";
    for (const auto& candidate : candidates) {
        const auto& bytes = candidate.bytes;
        const auto text_size = std::min(bytes.size(), settings_max_bytes);
        const std::string_view text(
            reinterpret_cast<const char*>(bytes.data()), text_size);
        const auto settings = gbb::parse_settings_text(text);
        const auto trace = gbb::parse_trace(text);
        const auto packet = gameboy::LinkPacketCodec::decode(
            bytes.data(), bytes.size());

        std::string save_state = "rejected";
        try {
            const auto state_size = std::min(
                bytes.size(), gameboy::save_state_container::maximum_state_size + 1U);
            const std::vector<std::uint8_t> state(bytes.begin(),
                                                  bytes.begin() + state_size);
            (void)gameboy::save_state_container::decode(state, 0);
            save_state = "accepted";
        } catch (const gameboy::SaveStateError&) {
        }

        std::array<std::uint8_t, sgb_packet_size> sgb_packet{};
        const auto copied = std::min(bytes.size(), sgb_packet.size());
        std::copy_n(bytes.begin(), copied, sgb_packet.begin());
        gameboy::Ppu ppu;
        ppu.set_sgb_mode(true);
        ppu.apply_sgb_command(sgb_packet, copied);

        std::cout << candidate.path.filename().string() << '\t'
                  << bytes.size() << '\t' << settings.entries.size() << '\t'
                  << (trace.valid() ? "yes" : "no") << '\t'
                  << trace.records.size() << '\t' << trace.errors.size() << '\t'
                  << (packet.has_value() ? "yes" : "no") << '\t' << save_state
                  << "\tapplied\n";
    }
    return 0;
}
