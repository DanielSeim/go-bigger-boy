#include "gameboy/gameshark.hpp"

#include "gameboy/memory_bus.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

namespace gameboy {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.resize(last + 1);
    return value;
}

std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::string quote(const std::string& value) {
    std::string result{"\""};
    for (const auto character : value) {
        if (character == '\\' || character == '"') result += '\\';
        result += character;
    }
    result += '"';
    return result;
}

unsigned hex_digit(const char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10U;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10U;
    throw std::invalid_argument("GameShark codes must contain hexadecimal digits");
}

std::uint8_t hex_byte(const std::string_view value) {
    return static_cast<std::uint8_t>((hex_digit(value[0]) << 4U) |
                                     hex_digit(value[1]));
}

struct PendingCheat {
    std::string description;
    std::string code;
    bool enabled{};
    bool from_archive{};
};

} // namespace

std::vector<GameSharkWrite> parse_gameshark_code(const std::string_view code) {
    std::vector<GameSharkWrite> writes;
    std::string token;
    const auto finish = [&] {
        if (token.empty()) return;
        if (token.size() != 8) {
            throw std::invalid_argument(
                "Each GameShark code must contain exactly 8 hexadecimal digits");
        }
        if (hex_byte(std::string_view{token}.substr(0, 2)) != 0x01) {
            throw std::invalid_argument(
                "Only Game Boy/Game Boy Color GameShark type-01 write codes are supported");
        }
        const auto value = hex_byte(std::string_view{token}.substr(2, 2));
        const auto low = hex_byte(std::string_view{token}.substr(4, 2));
        const auto high = hex_byte(std::string_view{token}.substr(6, 2));
        const auto address = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(high) << 8U) | low);
        if (address < 0x8000 || (address >= 0xFEA0 && address < 0xFF00) ||
            address == 0xFFFF) {
            throw std::invalid_argument(
                "GameShark type-01 code targets an unsupported address");
        }
        writes.push_back({address, value});
        token.clear();
    };
    for (const auto character : code) {
        if (std::isxdigit(static_cast<unsigned char>(character))) {
            token += static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        } else if (character == '+' || character == ',' ||
                   std::isspace(static_cast<unsigned char>(character))) {
            finish();
        } else if (character == '-') {
            // Hyphens are accepted as visual separators within a code.
        } else {
            throw std::invalid_argument("GameShark code contains an invalid character");
        }
    }
    finish();
    if (writes.empty()) throw std::invalid_argument("GameShark code is empty");
    return writes;
}

std::vector<GameSharkCheat> parse_libretro_cheats(const std::string_view text,
                                                   const bool from_archive) {
    std::map<std::size_t, PendingCheat> pending;
    std::istringstream input{std::string{text}};
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const auto key = trim(line.substr(0, separator));
        const auto value = unquote(line.substr(separator + 1));
        if (key.rfind("cheat", 0) != 0) continue;
        auto number_end = std::size_t{5};
        while (number_end < key.size() &&
               std::isdigit(static_cast<unsigned char>(key[number_end]))) {
            ++number_end;
        }
        if (number_end == 5) continue;
        const auto index = static_cast<std::size_t>(
            std::stoul(key.substr(5, number_end - 5)));
        const auto field = key.substr(number_end);
        auto& item = pending[index];
        if (field == "_desc") item.description = value;
        else if (field == "_code") item.code = value;
        else if (field == "_enable") item.enabled = value == "true" || value == "1";
        else if (field == "_source") item.from_archive = value == "archive";
    }
    std::vector<GameSharkCheat> cheats;
    for (auto& [index, item] : pending) {
        static_cast<void>(index);
        if (item.code.empty()) continue;
        try {
            auto writes = parse_gameshark_code(item.code);
            if (item.description.empty()) item.description = item.code;
            cheats.push_back({std::move(item.description), std::move(item.code),
                              item.enabled,
                              from_archive || item.from_archive,
                              std::move(writes)});
        } catch (const std::invalid_argument&) {
            // Libretro collections can contain codes for other cheat engines.
            // Skip those rather than presenting entries this core cannot apply.
        }
    }
    return cheats;
}

std::string serialize_libretro_cheats(
    const std::vector<GameSharkCheat>& cheats) {
    std::ostringstream output;
    output << "cheats = " << cheats.size() << "\n\n";
    for (std::size_t index = 0; index < cheats.size(); ++index) {
        const auto& cheat = cheats[index];
        output << "cheat" << index << "_desc = " << quote(cheat.description)
               << '\n'
               << "cheat" << index << "_code = " << quote(cheat.code) << '\n'
               << "cheat" << index << "_enable = "
               << (cheat.enabled ? "true" : "false") << '\n'
               << "cheat" << index << "_source = "
               << quote(cheat.from_archive ? "archive" : "manual") << "\n\n";
    }
    return output.str();
}

void apply_gameshark_cheats(const std::vector<GameSharkCheat>& cheats,
                            MemoryBus& bus) {
    for (const auto& cheat : cheats) {
        if (!cheat.enabled) continue;
        for (const auto& write : cheat.writes) bus.write8(write.address, write.value);
    }
}

} // namespace gameboy
