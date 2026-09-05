#define GBB_PLUGIN_BUILD
#include "gbb/plugin_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

#ifndef GBB_PLUGIN_FIXTURE_MODE
#define GBB_PLUGIN_FIXTURE_MODE 0
#endif

namespace {

#if GBB_PLUGIN_FIXTURE_MODE == 7
std::mutex step_mutex;
std::atomic<int> step_attempts{0};
#endif

constexpr std::uint32_t frame_width = 2;
constexpr std::uint32_t frame_height = 2;
constexpr std::uint32_t frame_pitch = frame_width * sizeof(std::uint32_t);
constexpr std::uint64_t frame_bytes = frame_pitch * frame_height;

struct FixtureCore {
    std::uint64_t fingerprint{};
    std::uint32_t steps{};
    std::uint8_t input_mask{};
    std::uint8_t frame[frame_bytes]{};
};

constexpr gbb_plugin_input_v1 inputs[] = {
    {GBB_PLUGIN_INPUT_A, "A"},
    {GBB_PLUGIN_INPUT_B, "B"},
};

constexpr gbb_plugin_descriptor_v1 descriptor{
    {sizeof(gbb_plugin_descriptor_v1), GBB_PLUGIN_ABI_MAJOR,
     GBB_PLUGIN_ABI_MINOR},
    "fixture", "GBB ABI fixture", GBB_PLUGIN_SYSTEM_GB, 0, 0, 0,
    frame_width, frame_height, 60000, 4194304, 4, 44100, 2, inputs, 2, 0,
    "ABI fixture ROM", 0, 0};

std::uint64_t fingerprint(const std::uint8_t* rom, const std::uint64_t size) {
    std::uint64_t value = UINT64_C(1469598103934665603);
    for (std::uint64_t index = 0; index < size; ++index) {
        value ^= rom[index];
        value *= UINT64_C(1099511628211);
    }
    return value;
}

bool header_supported(const gbb_plugin_struct_header& header,
                      const std::uint32_t required_size) {
    return header.struct_size >= required_size &&
           header.abi_major == GBB_PLUGIN_ABI_MAJOR &&
           header.abi_minor <= GBB_PLUGIN_ABI_MINOR;
}

FixtureCore* as_core(const gbb_plugin_core_handle handle) {
    return static_cast<FixtureCore*>(handle);
}

gbb_plugin_result GBB_PLUGIN_CALL create(
    const std::uint8_t* rom, const std::uint64_t rom_size,
    const gbb_plugin_create_options_v1*, gbb_plugin_core_handle* out_core) {
    if (rom == nullptr || rom_size == 0 || out_core == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
#if GBB_PLUGIN_FIXTURE_MODE == 6
    return GBB_PLUGIN_INTERNAL_ERROR;
#endif
    auto* core = new (std::nothrow) FixtureCore{};
    if (core == nullptr) return GBB_PLUGIN_INTERNAL_ERROR;
    core->fingerprint = fingerprint(rom, rom_size);
    std::memset(core->frame, 0x11, sizeof(core->frame));
    *out_core = core;
    return GBB_PLUGIN_OK;
}

void GBB_PLUGIN_CALL destroy(const gbb_plugin_core_handle handle) {
    delete as_core(handle);
}

gbb_plugin_result GBB_PLUGIN_CALL reset(const gbb_plugin_core_handle handle) {
    auto* core = as_core(handle);
    if (core == nullptr) return GBB_PLUGIN_INVALID_ARGUMENT;
    core->steps = 0;
    core->input_mask = 0;
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL step(const gbb_plugin_core_handle handle,
                                       std::uint32_t* out_cycles) {
    auto* core = as_core(handle);
    if (core == nullptr || out_cycles == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
#if GBB_PLUGIN_FIXTURE_MODE == 11
    try {
        throw std::runtime_error("fixture exception");
    } catch (...) {
        return GBB_PLUGIN_INTERNAL_ERROR;
    }
#endif
#if GBB_PLUGIN_FIXTURE_MODE == 7
    // Make the concurrency rejection deterministic even on runners that
    // schedule the two test threads sequentially. The first caller holds the
    // mutex until the second caller has attempted to enter it.
    const auto attempt = step_attempts.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (!step_mutex.try_lock()) return GBB_PLUGIN_INVALID_STATE;
    if (attempt == 1) {
        while (step_attempts.load(std::memory_order_acquire) < 2) {
            std::this_thread::yield();
        }
    }
#endif
    ++core->steps;
    *out_cycles = 4;
#if GBB_PLUGIN_FIXTURE_MODE == 7
    step_mutex.unlock();
#endif
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL frame_ready(
    const gbb_plugin_core_handle handle, std::uint8_t* out_ready) {
    if (as_core(handle) == nullptr || out_ready == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    *out_ready = 1;
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL consume_frame(
    const gbb_plugin_core_handle handle) {
    return as_core(handle) == nullptr ? GBB_PLUGIN_INVALID_ARGUMENT
                                      : GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL video_frame(
    const gbb_plugin_core_handle handle, std::uint8_t* buffer,
    const std::uint64_t capacity, gbb_plugin_video_frame_v1* out_frame) {
    if (as_core(handle) == nullptr || out_frame == nullptr ||
        !header_supported(out_frame->header, sizeof(*out_frame))) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    out_frame->width = frame_width;
    out_frame->height = frame_height;
    out_frame->pitch = frame_pitch;
    out_frame->pixel_format = GBB_PLUGIN_PIXEL_FORMAT_XRGB8888;
    out_frame->required_bytes = frame_bytes;
#if GBB_PLUGIN_FIXTURE_MODE == 4
    out_frame->width = frame_width + 1;
#endif
    if (buffer == nullptr || capacity < frame_bytes) {
        return GBB_PLUGIN_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, as_core(handle)->frame, frame_bytes);
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL audio_read(
    const gbb_plugin_core_handle handle, std::int16_t* samples,
    const std::uint64_t capacity, std::uint64_t* out_sample_count) {
    if (as_core(handle) == nullptr || out_sample_count == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    *out_sample_count = 2;
    if (samples == nullptr || capacity < 2) return GBB_PLUGIN_BUFFER_TOO_SMALL;
    samples[0] = 0;
    samples[1] = static_cast<std::int16_t>(as_core(handle)->input_mask);
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL set_input(
    const gbb_plugin_core_handle handle, const std::uint16_t input_id,
    const std::uint8_t pressed) {
    auto* core = as_core(handle);
    if (core == nullptr ||
        (input_id != GBB_PLUGIN_INPUT_A && input_id != GBB_PLUGIN_INPUT_B) ||
        pressed > 1) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    const auto mask = static_cast<std::uint8_t>(
        1U << (input_id == GBB_PLUGIN_INPUT_A ? 0 : 1));
    if (pressed != 0) core->input_mask |= mask;
    else core->input_mask &= static_cast<std::uint8_t>(~mask);
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL save_state(
    const gbb_plugin_core_handle handle, const gbb_plugin_host_v1* host,
    gbb_plugin_blob_v1* out_state) {
    auto* core = as_core(handle);
    if (core == nullptr || host == nullptr || out_state == nullptr ||
        !header_supported(host->header, sizeof(*host)) ||
        host->allocate == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
#if GBB_PLUGIN_FIXTURE_MODE == 5
    out_state->data = nullptr;
    out_state->size = 1;
    return GBB_PLUGIN_OK;
#endif
#if GBB_PLUGIN_FIXTURE_MODE == 12
    /* The reference host must reject a non-power-of-two alignment. */
    void* invalid_alignment = host->allocate(host->user_data, 1, 3);
    if (invalid_alignment != nullptr) {
        host->deallocate(host->user_data, invalid_alignment, 1, 3);
        return GBB_PLUGIN_INTERNAL_ERROR;
    }
#endif
    auto* data = static_cast<std::uint8_t*>(host->allocate(
        host->user_data, sizeof(*core), alignof(FixtureCore)));
    if (data == nullptr) return GBB_PLUGIN_INTERNAL_ERROR;
    std::memcpy(data, core, sizeof(*core));
    out_state->data = data;
    out_state->size = sizeof(*core);
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL load_state(
    const gbb_plugin_core_handle handle, const std::uint8_t* state,
    const std::uint64_t state_size) {
    auto* core = as_core(handle);
    if (core == nullptr || state == nullptr || state_size != sizeof(*core)) {
        return GBB_PLUGIN_INVALID_STATE;
    }
    std::memcpy(core, state, sizeof(*core));
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL rom_fingerprint(
    const gbb_plugin_core_handle handle, std::uint64_t* out_fingerprint) {
    if (as_core(handle) == nullptr || out_fingerprint == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    *out_fingerprint = as_core(handle)->fingerprint;
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL has_persistent_data(
    const gbb_plugin_core_handle handle, std::uint16_t,
    std::uint8_t* out_present) {
    if (as_core(handle) == nullptr || out_present == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    *out_present = 0;
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL read_persistent_data(
    const gbb_plugin_core_handle handle, std::uint16_t,
    const gbb_plugin_host_v1*, gbb_plugin_blob_v1*) {
    return as_core(handle) == nullptr ? GBB_PLUGIN_INVALID_ARGUMENT
                                      : GBB_PLUGIN_UNSUPPORTED;
}

gbb_plugin_result GBB_PLUGIN_CALL write_persistent_data(
    const gbb_plugin_core_handle handle, std::uint16_t, const std::uint8_t*,
    std::uint64_t) {
    return as_core(handle) == nullptr ? GBB_PLUGIN_INVALID_ARGUMENT
                                      : GBB_PLUGIN_UNSUPPORTED;
}

gbb_plugin_result GBB_PLUGIN_CALL flush_persistent_data(
    const gbb_plugin_core_handle handle) {
    return as_core(handle) == nullptr ? GBB_PLUGIN_INVALID_ARGUMENT
                                      : GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL release_blob(
    const gbb_plugin_core_handle handle, const gbb_plugin_host_v1* host,
    gbb_plugin_blob_v1* blob) {
    if (as_core(handle) == nullptr || host == nullptr || blob == nullptr ||
        host->deallocate == nullptr || blob->data == nullptr) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    host->deallocate(host->user_data, blob->data, blob->size,
                     alignof(FixtureCore));
    blob->data = nullptr;
    blob->size = 0;
    return GBB_PLUGIN_OK;
}

gbb_plugin_result GBB_PLUGIN_CALL query_extension(
    const gbb_plugin_core_handle handle, std::uint32_t, void*) {
    return as_core(handle) == nullptr ? GBB_PLUGIN_INVALID_ARGUMENT
                                      : GBB_PLUGIN_UNSUPPORTED;
}

} // namespace

extern "C" GBB_PLUGIN_EXPORT gbb_plugin_result GBB_PLUGIN_CALL gbb_plugin_query(
    const gbb_plugin_host_v1* host, gbb_plugin_v1* out_plugin) {
    if (host == nullptr || out_plugin == nullptr ||
        !header_supported(host->header, sizeof(*host)) ||
        host->allocate == nullptr || host->deallocate == nullptr ||
        !header_supported(out_plugin->header, GBB_PLUGIN_V1_REQUIRED_SIZE)) {
        return GBB_PLUGIN_INVALID_ARGUMENT;
    }
    out_plugin->descriptor = descriptor;
    out_plugin->core.header = {GBB_PLUGIN_CORE_API_V1_REQUIRED_SIZE,
                               GBB_PLUGIN_ABI_MAJOR, GBB_PLUGIN_ABI_MINOR};
    out_plugin->core.create = create;
    out_plugin->core.destroy = destroy;
    out_plugin->core.reset = reset;
    out_plugin->core.step_instruction = step;
    out_plugin->core.frame_ready = frame_ready;
    out_plugin->core.consume_frame = consume_frame;
    out_plugin->core.video_frame = video_frame;
    out_plugin->core.audio_read = audio_read;
    out_plugin->core.set_input = set_input;
    out_plugin->core.save_state = save_state;
    out_plugin->core.load_state = load_state;
    out_plugin->core.rom_fingerprint = rom_fingerprint;
    out_plugin->core.has_persistent_data = has_persistent_data;
    out_plugin->core.read_persistent_data = read_persistent_data;
    out_plugin->core.write_persistent_data = write_persistent_data;
    out_plugin->core.flush_persistent_data = flush_persistent_data;
    out_plugin->core.release_blob = release_blob;
    out_plugin->core.query_extension = query_extension;
#if GBB_PLUGIN_FIXTURE_MODE == 1
    out_plugin->descriptor.capabilities = UINT64_C(1) << 63;
#elif GBB_PLUGIN_FIXTURE_MODE == 2
    out_plugin->core.query_extension = nullptr;
#elif GBB_PLUGIN_FIXTURE_MODE == 3
    out_plugin->core.header.struct_size = sizeof(gbb_plugin_struct_header);
#elif GBB_PLUGIN_FIXTURE_MODE == 9
    static const char invalid_utf8[] = "\xc0\x80";
    out_plugin->descriptor.core_name = invalid_utf8;
#elif GBB_PLUGIN_FIXTURE_MODE == 10
    static char unterminated_name[4096];
    for (auto& byte : unterminated_name) byte = 'x';
    out_plugin->descriptor.core_name = unterminated_name;
#endif
    out_plugin->header = {sizeof(gbb_plugin_v1), GBB_PLUGIN_ABI_MAJOR,
                          GBB_PLUGIN_ABI_MINOR};
#if GBB_PLUGIN_FIXTURE_MODE == 8
    out_plugin->header.struct_size += 16;
    out_plugin->descriptor.header.struct_size += 16;
    out_plugin->core.header.struct_size += 16;
#endif
    return GBB_PLUGIN_OK;
}
