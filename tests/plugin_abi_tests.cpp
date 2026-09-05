#include "gbb/plugin_abi.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

int failures = 0;

void check(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void* GBB_PLUGIN_CALL allocate(void*, const std::uint64_t size,
                               std::uint64_t) {
    return std::malloc(static_cast<std::size_t>(size));
}

void GBB_PLUGIN_CALL deallocate(void*, void* pointer, std::uint64_t,
                                std::uint64_t) {
    std::free(pointer);
}

void GBB_PLUGIN_CALL log_message(void*, std::uint32_t, const char*) {}

#if defined(_WIN32)
using Library = HMODULE;
Library open_library(const char* path) { return LoadLibraryA(path); }
void close_library(const Library library) { FreeLibrary(library); }
gbb_plugin_query_fn load_query(const Library library) {
    return reinterpret_cast<gbb_plugin_query_fn>(
        GetProcAddress(library, "gbb_plugin_query"));
}
#else
using Library = void*;
Library open_library(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void close_library(const Library library) { dlclose(library); }
gbb_plugin_query_fn load_query(const Library library) {
    return reinterpret_cast<gbb_plugin_query_fn>(dlsym(library, "gbb_plugin_query"));
}
#endif

gbb_plugin_host_v1 host() {
    return {{sizeof(gbb_plugin_host_v1), GBB_PLUGIN_ABI_MAJOR,
             GBB_PLUGIN_ABI_MINOR},
            nullptr, allocate, deallocate, log_message};
}

gbb_plugin_v1 empty_plugin() {
    gbb_plugin_v1 plugin{};
    plugin.header = {GBB_PLUGIN_V1_REQUIRED_SIZE, GBB_PLUGIN_ABI_MAJOR,
                     GBB_PLUGIN_ABI_MINOR};
    return plugin;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: gameboy_plugin_abi_tests FIXTURE_LIBRARY\n";
        return 2;
    }
    const auto library = open_library(argv[1]);
    check(library != nullptr, "fixture shared library loads");
    if (library == nullptr) return 1;
    const auto query = load_query(library);
    check(query != nullptr, "fixture exports the C query symbol");
    if (query == nullptr) {
        close_library(library);
        return 1;
    }

    auto fixture_host = host();
    auto plugin = empty_plugin();
    const auto query_result = query(&fixture_host, &plugin);
    check(query_result == GBB_PLUGIN_OK,
          "host and plugin negotiate ABI v1");
    if (query_result != GBB_PLUGIN_OK) {
        close_library(library);
        return 1;
    }
    check(plugin.descriptor.core_id != nullptr &&
              std::string(plugin.descriptor.core_id) == "fixture" &&
              plugin.descriptor.video_width == 2 &&
              plugin.descriptor.video_height == 2 &&
              plugin.descriptor.input_count == 2,
          "fixture descriptor uses fixed-width metadata");
    check(plugin.descriptor.header.abi_major == GBB_PLUGIN_ABI_MAJOR &&
              plugin.core.header.abi_major == GBB_PLUGIN_ABI_MAJOR &&
              plugin.core.header.struct_size >= sizeof(gbb_plugin_core_api_v1),
          "descriptor and function table report compatible versions");
    check(plugin.core.create != nullptr && plugin.core.destroy != nullptr &&
              plugin.core.video_frame != nullptr &&
              plugin.core.save_state != nullptr &&
              plugin.core.flush_persistent_data != nullptr,
          "fixture exposes the required core operations");

    auto bad_host = fixture_host;
    bad_host.header.abi_major = 2;
    auto rejected_plugin = empty_plugin();
    check(query(&bad_host, &rejected_plugin) != GBB_PLUGIN_OK,
          "unsupported host major version is rejected");
    bad_host = fixture_host;
    bad_host.header.abi_minor = GBB_PLUGIN_ABI_MINOR + 1;
    rejected_plugin = empty_plugin();
    check(query(&bad_host, &rejected_plugin) != GBB_PLUGIN_OK,
          "newer host minor version is rejected conservatively");
    auto short_plugin = empty_plugin();
    short_plugin.header.struct_size = sizeof(gbb_plugin_struct_header);
    check(query(&fixture_host, &short_plugin) != GBB_PLUGIN_OK,
          "truncated plugin output table is rejected");

    alignas(gbb_plugin_v1)
        std::array<std::uint8_t, sizeof(gbb_plugin_v1) + 16> oversized_storage{};
    std::fill(oversized_storage.begin(), oversized_storage.end(), 0xa5);
    auto* oversized_plugin = reinterpret_cast<gbb_plugin_v1*>(
        oversized_storage.data());
    oversized_plugin->header = {GBB_PLUGIN_V1_REQUIRED_SIZE + 16,
                                GBB_PLUGIN_ABI_MAJOR, GBB_PLUGIN_ABI_MINOR};
    check(query(&fixture_host, oversized_plugin) == GBB_PLUGIN_OK,
          "append-only oversized output table is accepted");
    check(std::all_of(oversized_storage.begin() + sizeof(gbb_plugin_v1),
                      oversized_storage.end(),
                      [](const std::uint8_t value) { return value == 0xa5; }),
          "unknown appended output fields remain untouched");

    const std::vector<std::uint8_t> rom{1, 2, 3, 4};
    gbb_plugin_core_handle core = nullptr;
    check(plugin.core.create(rom.data(), rom.size(), nullptr, &core) ==
              GBB_PLUGIN_OK &&
              core != nullptr,
          "fixture creates an opaque core handle");
    if (core != nullptr) {
        std::uint64_t fingerprint = 0;
        check(plugin.core.rom_fingerprint(core, &fingerprint) == GBB_PLUGIN_OK &&
                  fingerprint != 0,
              "fixture exposes a stable ROM fingerprint");
        std::uint32_t cycles = 0;
        check(plugin.core.step_instruction(core, &cycles) == GBB_PLUGIN_OK &&
                  cycles == 4,
              "fixture steps through the C ABI");
        std::uint8_t ready = 0;
        check(plugin.core.frame_ready(core, &ready) == GBB_PLUGIN_OK && ready,
              "fixture reports frame readiness");

        gbb_plugin_video_frame_v1 frame{{sizeof(frame), GBB_PLUGIN_ABI_MAJOR,
                                         GBB_PLUGIN_ABI_MINOR}};
        check(plugin.core.video_frame(core, nullptr, 0, &frame) ==
                  GBB_PLUGIN_BUFFER_TOO_SMALL &&
                  frame.required_bytes == 16,
              "video operation reports required caller buffer size");
        std::array<std::uint8_t, 16> pixels{};
        check(plugin.core.video_frame(core, pixels.data(), pixels.size(),
                                     &frame) == GBB_PLUGIN_OK &&
                  pixels[0] == 0x11,
              "video operation writes into host-owned memory");

        std::uint64_t sample_count = 0;
        check(plugin.core.audio_read(core, nullptr, 0, &sample_count) ==
                  GBB_PLUGIN_BUFFER_TOO_SMALL && sample_count == 2,
              "audio operation reports required sample capacity");
        std::int16_t samples[2]{};
        check(plugin.core.set_input(core, GBB_PLUGIN_INPUT_A, 1) ==
                      GBB_PLUGIN_OK &&
                  plugin.core.audio_read(core, samples, 2, &sample_count) ==
                      GBB_PLUGIN_OK &&
                  samples[1] == 1,
              "input and audio operations share the opaque handle");

        gbb_plugin_blob_v1 state{};
        check(plugin.core.save_state(core, &fixture_host, &state) ==
                  GBB_PLUGIN_OK && state.data != nullptr && state.size != 0,
              "save state uses the host allocator");
        check(plugin.core.reset(core) == GBB_PLUGIN_OK &&
                  plugin.core.load_state(core, state.data, state.size) ==
                      GBB_PLUGIN_OK,
              "save state round trips through the C ABI");
        sample_count = 0;
        check(plugin.core.audio_read(core, samples, 2, &sample_count) ==
                  GBB_PLUGIN_OK && samples[1] == 1,
              "loaded state restores opaque core data");
        check(plugin.core.release_blob(core, &fixture_host, &state) ==
                  GBB_PLUGIN_OK && state.data == nullptr,
              "host-owned save-state memory is released explicitly");
        check(plugin.core.release_blob(core, &fixture_host, &state) ==
                  GBB_PLUGIN_INVALID_ARGUMENT,
              "released blobs cannot be released twice");

        std::uint8_t present = 1;
        check(plugin.core.has_persistent_data(core, 0, &present) ==
                  GBB_PLUGIN_OK && present == 0,
              "persistent-data capability is explicit");
        check(plugin.core.flush_persistent_data(core) == GBB_PLUGIN_OK,
              "persistent-data flushing has an explicit operation");
        check(plugin.core.query_extension(core, 99, nullptr) ==
                  GBB_PLUGIN_UNSUPPORTED,
              "unknown optional extensions fail closed");
        plugin.core.destroy(core);
    }
    close_library(library);
    return failures == 0 ? 0 : 1;
}
