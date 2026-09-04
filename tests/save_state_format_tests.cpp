#include "save_state_container.hpp"
#include "save_state_format.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

std::uint32_t next_value(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_save_state_error(Function&& function, const char* message) {
    try {
        function();
    } catch (const gameboy::SaveStateError&) {
        return;
    }
    check(false, message);
}

void test_round_trip_and_wire_order() {
    gameboy::save_state_format::Writer writer;
    writer.u8(0x12);
    writer.boolean(true);
    writer.u16(0x3456);
    writer.u32(0x789ABCDE);
    writer.u64(UINT64_C(0x0123456789ABCDEF));
    writer.f32(1.5F);
    writer.string("GBB");

    const auto& bytes = writer.data();
    check(bytes.size() == 1 + 1 + 2 + 4 + 8 + 4 + 4 + 3,
          "writer emits the expected field sizes");
    check(bytes[0] == 0x12 && bytes[1] == 1 && bytes[2] == 0x56 &&
              bytes[3] == 0x34 && bytes[4] == 0xDE && bytes[5] == 0xBC &&
              bytes[6] == 0x9A && bytes[7] == 0x78,
          "writer uses little-endian integer order");

    gameboy::save_state_format::Reader reader(bytes);
    check(reader.u8() == 0x12 && reader.boolean() && reader.u16() == 0x3456 &&
              reader.u32() == 0x789ABCDE &&
              reader.u64() == UINT64_C(0x0123456789ABCDEF) &&
              reader.f32() == 1.5F && reader.string() == "GBB",
          "reader round-trips all primitive fields");
    reader.finish();
}

void test_validation_and_crc() {
    const std::vector<std::uint8_t> bytes{0, 1, 2, 3};
    check(gameboy::save_state_format::crc32(
              reinterpret_cast<const std::uint8_t*>("123456789"), 9) ==
              UINT32_C(0xCBF43926),
          "CRC32 matches the standard check vector");

    gameboy::save_state_format::Reader bounded(bytes, 1, 2);
    check(bounded.u8() == 1 && bounded.u8() == 2,
          "reader honors a bounded subrange");
    bounded.finish();

    expect_save_state_error(
        [&] { gameboy::save_state_format::Reader invalid(bytes, 5); },
        "reader rejects a start offset beyond the input");
    expect_save_state_error(
        [&] { gameboy::save_state_format::Reader invalid(bytes, 3, 2); },
        "reader rejects a subrange beyond the input");
    expect_save_state_error(
        [&] {
            const std::vector<std::uint8_t> invalid_bytes{2};
            gameboy::save_state_format::Reader invalid(invalid_bytes);
            (void)invalid.boolean();
        },
        "reader rejects invalid boolean values");
    expect_save_state_error(
        [&] {
            const std::vector<std::uint8_t> invalid_bytes;
            gameboy::save_state_format::Reader invalid(invalid_bytes);
            (void)invalid.u8();
        },
        "reader rejects truncated fields");

    gameboy::save_state_format::Writer oversized;
    expect_save_state_error(
        [&] {
            oversized.string(std::string(
                gameboy::save_state_format::maximum_serial_output + 1, 'x'));
        },
        "writer rejects oversized strings");
}

void test_deterministic_primitive_round_trips() {
    std::uint32_t state = 0x5A17E5U;
    for (unsigned index = 0; index < 128; ++index) {
        const auto u8 = static_cast<std::uint8_t>(next_value(state));
        const auto boolean = (next_value(state) & 1U) != 0;
        const auto u16 = static_cast<std::uint16_t>(next_value(state));
        const auto u32 = next_value(state);
        const auto u64 = (static_cast<std::uint64_t>(next_value(state)) << 32) |
                         next_value(state);
        const auto text = std::string("state-") + std::to_string(index) +
                          '-' + std::to_string(u32);

        gameboy::save_state_format::Writer writer;
        writer.u8(u8);
        writer.boolean(boolean);
        writer.u16(u16);
        writer.u32(u32);
        writer.u64(u64);
        writer.string(text);

        gameboy::save_state_format::Reader reader(writer.data());
        check(reader.u8() == u8 && reader.boolean() == boolean &&
                  reader.u16() == u16 && reader.u32() == u32 &&
                  reader.u64() == u64 && reader.string() == text,
              "generated save-state primitives round-trip");
        reader.finish();
    }
}

void test_container_round_trip_and_validation() {
    constexpr std::uint64_t fingerprint = UINT64_C(0x0123456789ABCDEF);
    const std::vector<std::uint8_t> payload{0x10, 0x20, 0x30, 0x40};
    const auto state = gameboy::save_state_container::encode(fingerprint,
                                                               payload);
    check(state.size() == gameboy::save_state_container::header_size +
                              payload.size(),
          "save-state container emits a fixed header and payload");

    const auto decoded = gameboy::save_state_container::decode(state,
                                                                 fingerprint);
    check(decoded.version == gameboy::save_state_container::current_version &&
              decoded.payload == payload,
          "save-state container round-trips version and payload");

    expect_save_state_error(
        [&] {
            (void)gameboy::save_state_container::decode(
                state, fingerprint ^ UINT64_C(1));
        },
        "save-state container rejects a different ROM fingerprint");

    auto corrupt = state;
    corrupt.back() ^= 0x01;
    expect_save_state_error(
        [&] {
            (void)gameboy::save_state_container::decode(corrupt, fingerprint);
        },
        "save-state container rejects a checksum mismatch");

    for (std::size_t index = 0; index < state.size(); ++index) {
        // Version bytes may legitimately select another supported format;
        // integrity mutations elsewhere must always be rejected.
        if (index >= 8 && index < 12) continue;
        auto mutated = state;
        mutated[index] ^= 0x01;
        expect_save_state_error(
            [&] {
                (void)gameboy::save_state_container::decode(mutated, fingerprint);
            },
            "save-state container rejects single-bit mutations");
    }

    auto truncated = state;
    truncated.pop_back();
    expect_save_state_error(
        [&] {
            (void)gameboy::save_state_container::decode(truncated, fingerprint);
        },
        "save-state container rejects a truncated payload");

    auto invalid_size = state;
    invalid_size[20] = 0;
    invalid_size[21] = 0;
    invalid_size[22] = 0;
    invalid_size[23] = 0;
    expect_save_state_error(
        [&] {
            (void)gameboy::save_state_container::decode(invalid_size,
                                                        fingerprint);
        },
        "save-state container rejects a mismatched payload size");
}

} // namespace

int main() {
    test_round_trip_and_wire_order();
    test_validation_and_crc();
    test_deterministic_primitive_round_trips();
    test_container_round_trip_and_validation();
    return failures == 0 ? 0 : 1;
}
