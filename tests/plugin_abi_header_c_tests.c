#include "gbb/plugin_abi.h"

#include <stddef.h>

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(gbb_plugin_host_v1) == 40,
               "v1 host table layout changed");
_Static_assert(sizeof(gbb_plugin_descriptor_v1) == 120,
               "v1 descriptor layout changed");
_Static_assert(sizeof(gbb_plugin_create_options_v1) == 24,
               "v1 create options layout changed");
_Static_assert(sizeof(gbb_plugin_blob_v1) == 16,
               "v1 blob layout changed");
_Static_assert(sizeof(gbb_plugin_video_frame_v1) == 32,
               "v1 video frame layout changed");
_Static_assert(sizeof(gbb_plugin_core_api_v1) == 152,
               "v1 core table layout changed");
_Static_assert(sizeof(gbb_plugin_v1) == 280,
               "v1 plugin table layout changed");
_Static_assert(offsetof(gbb_plugin_host_v1, allocate) == 16,
               "v1 host allocator offset changed");
_Static_assert(offsetof(gbb_plugin_descriptor_v1, inputs) == 72,
               "v1 descriptor input offset changed");
_Static_assert(offsetof(gbb_plugin_descriptor_v1, capabilities) == 88,
               "v1 descriptor capability offset changed");
_Static_assert(offsetof(gbb_plugin_core_api_v1, flush_persistent_data) == 128,
               "v1 flush callback offset changed");
#endif

/* Numeric identifiers are part of the frozen v1.0 wire contract. */
_Static_assert(GBB_PLUGIN_ABI_MAJOR == 1 && GBB_PLUGIN_ABI_MINOR == 0,
               "v1.0 ABI version changed");
_Static_assert(GBB_PLUGIN_OK == 0 && GBB_PLUGIN_INVALID_ARGUMENT == 1 &&
                   GBB_PLUGIN_UNSUPPORTED == 2 &&
                   GBB_PLUGIN_BUFFER_TOO_SMALL == 3 &&
                   GBB_PLUGIN_INVALID_STATE == 4 &&
                   GBB_PLUGIN_INTERNAL_ERROR == 5 && GBB_PLUGIN_FATAL == 6,
               "v1.0 result identifiers changed");
_Static_assert(GBB_PLUGIN_SYSTEM_GB == 0 && GBB_PLUGIN_SYSTEM_GBC == 1 &&
                   GBB_PLUGIN_SYSTEM_GBA == 2,
               "v1.0 system identifiers changed");
_Static_assert(GBB_PLUGIN_INPUT_RIGHT == 0 && GBB_PLUGIN_INPUT_LEFT == 1 &&
                   GBB_PLUGIN_INPUT_UP == 2 && GBB_PLUGIN_INPUT_DOWN == 3 &&
                   GBB_PLUGIN_INPUT_A == 4 && GBB_PLUGIN_INPUT_B == 5 &&
                   GBB_PLUGIN_INPUT_X == 6 && GBB_PLUGIN_INPUT_Y == 7 &&
                   GBB_PLUGIN_INPUT_L == 8 && GBB_PLUGIN_INPUT_R == 9 &&
                   GBB_PLUGIN_INPUT_SELECT == 10 &&
                   GBB_PLUGIN_INPUT_START == 11,
               "v1.0 input identifiers changed");
_Static_assert(GBB_PLUGIN_CAP_PERSISTENT_MEMORY == (UINT64_C(1) << 0) &&
                   GBB_PLUGIN_CAP_LINK_CABLE == (UINT64_C(1) << 10),
               "v1.0 capability identifiers changed");

int main(void) {
    gbb_plugin_struct_header header = {
        sizeof(gbb_plugin_struct_header), GBB_PLUGIN_ABI_MAJOR,
        GBB_PLUGIN_ABI_MINOR};
    gbb_plugin_result result = GBB_PLUGIN_OK;
    gbb_plugin_query_fn query = NULL;

    return header.struct_size == sizeof(header) && result == GBB_PLUGIN_OK &&
                   query == NULL && sizeof(gbb_plugin_result) == 4 &&
                   sizeof(gbb_plugin_system_id) == 1 &&
                   sizeof(gbb_plugin_pixel_format) == 4 &&
                   sizeof(gbb_plugin_persistent_kind) == 2
               ? 0
               : 1;
}
