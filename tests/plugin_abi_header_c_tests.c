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
