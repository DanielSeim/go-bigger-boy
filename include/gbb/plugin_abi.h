#ifndef GBB_PLUGIN_ABI_H
#define GBB_PLUGIN_ABI_H

/*
 * Go Bigger Boy dynamic-core ABI v1.0 (frozen).
 *
 * This header is deliberately C-compatible. Do not expose EmulatorCore or
 * any C++ standard-library type from a shared library. The v1.0 numeric IDs,
 * field order, and required prefixes are frozen. Every structure is
 * size-prefixed so a future minor revision can append fields without changing
 * the meaning of older fields.
 */
#include <stdint.h>

#if defined(_WIN32)
#define GBB_PLUGIN_CALL __cdecl
#if defined(GBB_PLUGIN_BUILD)
#define GBB_PLUGIN_EXPORT __declspec(dllexport)
#else
#define GBB_PLUGIN_EXPORT __declspec(dllimport)
#endif
#else
#define GBB_PLUGIN_CALL
#define GBB_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define GBB_PLUGIN_ABI_MAJOR UINT16_C(1)
/* Frozen v1.0. Append-only changes require a separately approved minor. */
#define GBB_PLUGIN_ABI_MINOR UINT16_C(0)
#define GBB_PLUGIN_MAX_STRING_BYTES UINT32_C(4096)

typedef struct gbb_plugin_struct_header {
    uint32_t struct_size;
    uint16_t abi_major;
    uint16_t abi_minor;
} gbb_plugin_struct_header;

/* Keep the function return type fixed-width; C enum widths are implementation
 * defined and therefore unsuitable for a binary boundary. */
typedef int32_t gbb_plugin_result;
enum {
    /* v1.0 result values are frozen; append new values only in a new minor. */
    GBB_PLUGIN_OK = INT32_C(0),
    GBB_PLUGIN_INVALID_ARGUMENT = INT32_C(1),
    GBB_PLUGIN_UNSUPPORTED = INT32_C(2),
    GBB_PLUGIN_BUFFER_TOO_SMALL = INT32_C(3),
    GBB_PLUGIN_INVALID_STATE = INT32_C(4),
    GBB_PLUGIN_INTERNAL_ERROR = INT32_C(5),
    GBB_PLUGIN_FATAL = INT32_C(6),
};

typedef uint8_t gbb_plugin_system_id;
enum {
    GBB_PLUGIN_SYSTEM_GB = 0,
    GBB_PLUGIN_SYSTEM_GBC = 1,
    GBB_PLUGIN_SYSTEM_GBA = 2,
};

typedef uint32_t gbb_plugin_pixel_format;
enum {
    GBB_PLUGIN_PIXEL_FORMAT_XRGB8888 = 1,
};

typedef uint16_t gbb_plugin_persistent_kind;
enum {
    GBB_PLUGIN_PERSISTENT_BATTERY_RAM = 0,
    GBB_PLUGIN_PERSISTENT_BATTERY_SAVE = 1,
    GBB_PLUGIN_PERSISTENT_RTC = 2,
};

/* Input IDs intentionally mirror gbb::InputId. They are part of the ABI so
 * adapters can map controls without comparing localized display names. */
enum {
    GBB_PLUGIN_INPUT_RIGHT = 0,
    GBB_PLUGIN_INPUT_LEFT = 1,
    GBB_PLUGIN_INPUT_UP = 2,
    GBB_PLUGIN_INPUT_DOWN = 3,
    GBB_PLUGIN_INPUT_A = 4,
    GBB_PLUGIN_INPUT_B = 5,
    GBB_PLUGIN_INPUT_X = 6,
    GBB_PLUGIN_INPUT_Y = 7,
    GBB_PLUGIN_INPUT_L = 8,
    GBB_PLUGIN_INPUT_R = 9,
    GBB_PLUGIN_INPUT_SELECT = 10,
    GBB_PLUGIN_INPUT_START = 11,
};

enum {
    GBB_PLUGIN_CAP_PERSISTENT_MEMORY = UINT64_C(1) << 0,
    GBB_PLUGIN_CAP_RTC = UINT64_C(1) << 1,
    GBB_PLUGIN_CAP_RUMBLE = UINT64_C(1) << 2,
    GBB_PLUGIN_CAP_CAMERA = UINT64_C(1) << 3,
    GBB_PLUGIN_CAP_PRINTER = UINT64_C(1) << 4,
    GBB_PLUGIN_CAP_COMPATIBILITY_PALETTE = UINT64_C(1) << 5,
    GBB_PLUGIN_CAP_CHEATS = UINT64_C(1) << 6,
    GBB_PLUGIN_CAP_DEBUGGER = UINT64_C(1) << 7,
    GBB_PLUGIN_CAP_SPRITE_EDITOR = UINT64_C(1) << 8,
    GBB_PLUGIN_CAP_SCENE_LAYERS = UINT64_C(1) << 9,
    GBB_PLUGIN_CAP_LINK_CABLE = UINT64_C(1) << 10,
};

#ifdef __cplusplus
extern "C" {
#endif

typedef void*(GBB_PLUGIN_CALL *gbb_plugin_allocate_fn)(
    void* user_data, uint64_t size, uint64_t alignment);
typedef void(GBB_PLUGIN_CALL *gbb_plugin_deallocate_fn)(
    void* user_data, void* pointer, uint64_t size, uint64_t alignment);
/* The level argument uses the GBB_PLUGIN_LOG_* constants below. */
typedef void(GBB_PLUGIN_CALL *gbb_plugin_log_fn)(
    void* user_data, uint32_t level, const char* message);

enum {
    GBB_PLUGIN_LOG_ERROR = 0,
    GBB_PLUGIN_LOG_WARNING = 1,
    GBB_PLUGIN_LOG_INFO = 2,
    GBB_PLUGIN_LOG_DEBUG = 3,
    GBB_PLUGIN_LOG_TRACE = 4,
};

/* Allocators use byte sizes and power-of-two alignments. An alignment of zero
 * requests the host's default max alignment; non-zero alignments must be at
 * least sizeof(void*) or allocation fails. A zero-size allocation returns
 * NULL. The same size/alignment pair is supplied to deallocate. */

typedef struct gbb_plugin_host_v1 {
    gbb_plugin_struct_header header;
    void* user_data;
    gbb_plugin_allocate_fn allocate;
    gbb_plugin_deallocate_fn deallocate;
    gbb_plugin_log_fn log;
} gbb_plugin_host_v1;

typedef struct gbb_plugin_input_v1 {
    uint16_t id;
    const char* name;
} gbb_plugin_input_v1;

typedef struct gbb_plugin_descriptor_v1 {
    gbb_plugin_struct_header header;
    const char* core_id;
    const char* core_name;
    gbb_plugin_system_id system_id;
    uint8_t supports_color;
    uint8_t requires_color;
    uint8_t has_battery;
    uint32_t video_width;
    uint32_t video_height;
    uint64_t refresh_rate_millihz;
    uint64_t clock_rate_hz;
    uint32_t nominal_cycles_per_frame;
    uint32_t audio_sample_rate;
    uint32_t audio_channels;
    const gbb_plugin_input_v1* inputs;
    uint32_t input_count;
    uint64_t capabilities;
    const char* software_title;
    uint64_t rom_size;
    uint64_t save_ram_size;
} gbb_plugin_descriptor_v1;

typedef struct gbb_plugin_create_options_v1 {
    gbb_plugin_struct_header header;
    const char* source_path;
    const char* persistence_path;
} gbb_plugin_create_options_v1;

typedef struct gbb_plugin_blob_v1 {
    uint8_t* data;
    uint64_t size;
} gbb_plugin_blob_v1;

typedef struct gbb_plugin_video_frame_v1 {
    gbb_plugin_struct_header header;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    gbb_plugin_pixel_format pixel_format;
    uint64_t required_bytes;
} gbb_plugin_video_frame_v1;

typedef void* gbb_plugin_core_handle;

typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_create_fn)(
    const uint8_t* rom, uint64_t rom_size,
    const gbb_plugin_create_options_v1* options,
    gbb_plugin_core_handle* out_core);
typedef void(GBB_PLUGIN_CALL *gbb_plugin_destroy_fn)(
    gbb_plugin_core_handle core);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_reset_fn)(
    gbb_plugin_core_handle core);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_step_fn)(
    gbb_plugin_core_handle core, uint32_t* out_cycles);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_frame_ready_fn)(
    gbb_plugin_core_handle core, uint8_t* out_ready);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_consume_frame_fn)(
    gbb_plugin_core_handle core);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_video_frame_fn)(
    gbb_plugin_core_handle core, uint8_t* buffer, uint64_t capacity,
    gbb_plugin_video_frame_v1* out_frame);
/* capacity and out_sample_count are counts of interleaved int16 samples, not
 * frames. A zero-capacity call may query the required sample count. Counts
 * returned by a conforming plug-in are multiples of descriptor audio_channels. */
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_audio_read_fn)(
    gbb_plugin_core_handle core, int16_t* samples, uint64_t capacity,
    uint64_t* out_sample_count);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_set_input_fn)(
    gbb_plugin_core_handle core, uint16_t input_id, uint8_t pressed);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_save_state_fn)(
    gbb_plugin_core_handle core, const gbb_plugin_host_v1* host,
    gbb_plugin_blob_v1* out_state);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_load_state_fn)(
    gbb_plugin_core_handle core, const uint8_t* state, uint64_t state_size);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_rom_fingerprint_fn)(
    gbb_plugin_core_handle core, uint64_t* out_fingerprint);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_has_persistent_data_fn)(
    gbb_plugin_core_handle core, gbb_plugin_persistent_kind kind,
    uint8_t* out_present);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_read_persistent_data_fn)(
    gbb_plugin_core_handle core, gbb_plugin_persistent_kind kind,
    const gbb_plugin_host_v1* host, gbb_plugin_blob_v1* out_data);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_write_persistent_data_fn)(
    gbb_plugin_core_handle core, gbb_plugin_persistent_kind kind,
    const uint8_t* data, uint64_t data_size);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_flush_persistent_data_fn)(
    gbb_plugin_core_handle core);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_release_blob_fn)(
    gbb_plugin_core_handle core, const gbb_plugin_host_v1* host,
    gbb_plugin_blob_v1* blob);
typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_query_extension_fn)(
    gbb_plugin_core_handle core, uint32_t extension_id, void* extension_table);

typedef struct gbb_plugin_core_api_v1 {
    gbb_plugin_struct_header header;
    gbb_plugin_create_fn create;
    gbb_plugin_destroy_fn destroy;
    gbb_plugin_reset_fn reset;
    gbb_plugin_step_fn step_instruction;
    gbb_plugin_frame_ready_fn frame_ready;
    gbb_plugin_consume_frame_fn consume_frame;
    gbb_plugin_video_frame_fn video_frame;
    gbb_plugin_audio_read_fn audio_read;
    gbb_plugin_set_input_fn set_input;
    gbb_plugin_save_state_fn save_state;
    gbb_plugin_load_state_fn load_state;
    gbb_plugin_rom_fingerprint_fn rom_fingerprint;
    gbb_plugin_has_persistent_data_fn has_persistent_data;
    gbb_plugin_read_persistent_data_fn read_persistent_data;
    gbb_plugin_write_persistent_data_fn write_persistent_data;
    gbb_plugin_flush_persistent_data_fn flush_persistent_data;
    gbb_plugin_release_blob_fn release_blob;
    gbb_plugin_query_extension_fn query_extension;
} gbb_plugin_core_api_v1;

typedef struct gbb_plugin_v1 {
    gbb_plugin_struct_header header;
    gbb_plugin_descriptor_v1 descriptor;
    gbb_plugin_core_api_v1 core;
} gbb_plugin_v1;

/* Minimum storage required by ABI v1. Future minor revisions append fields
 * after this boundary and must only write fields present in the host's
 * reported struct_size. */
#define GBB_PLUGIN_V1_REQUIRED_SIZE ((uint32_t)sizeof(gbb_plugin_v1))
#define GBB_PLUGIN_CORE_API_V1_REQUIRED_SIZE \
    ((uint32_t)sizeof(gbb_plugin_core_api_v1))

typedef gbb_plugin_result(GBB_PLUGIN_CALL *gbb_plugin_query_fn)(
    const gbb_plugin_host_v1* host, gbb_plugin_v1* out_plugin);

GBB_PLUGIN_EXPORT gbb_plugin_result GBB_PLUGIN_CALL gbb_plugin_query(
    const gbb_plugin_host_v1* host, gbb_plugin_v1* out_plugin);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GBB_PLUGIN_ABI_H */
