/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded, read-only host snapshots for authored native CGB access witnesses.
 */
#ifndef GBB_PIXEL_ACCESS_TRACE_H
#define GBB_PIXEL_ACCESS_TRACE_H
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#define PIXEL_ACCESS_MAX_STEPS 512
#define PIXEL_ACCESS_CLOCK_LIMIT UINT64_C(20000000)
typedef struct {
    uint64_t clock;
    uint16_t pc, af, bc, de, hl, sp;
    unsigned ly, mode, vram, palette;
    int frontier;
} PixelAccessSample;
static inline int pixel_access_frontier(const uint32_t *pixels, unsigned ly) {
    if (ly >= 144) return -1;
    unsigned x = 0;
    while (x < 160 && (pixels[ly*160+x]&0xffffff)==0x080000) ++x;
    for (unsigned i=x;i<160;++i)
        if ((pixels[ly*160+i]&0xffffff)!=0xffffff) return -2;
    return (int)x;
}
static inline uint64_t pixel_access_hash(const uint8_t *data, size_t count) {
    uint64_t value=UINT64_C(14695981039346656037);
    for (size_t i=0;i<count;++i) { value^=data[i]; value*=UINT64_C(1099511628211); }
    return value;
}
static inline void pixel_access_print_sample(const PixelAccessSample *s) {
    printf("{\"clock\":%" PRIu64 ",\"pc\":%u,\"af\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,\"sp\":%u,"
           "\"ly\":%u,\"mode\":%u,\"vram\":%u,\"palette\":%u,\"frontier\":%d}",
           s->clock,s->pc,s->af,s->bc,s->de,s->hl,s->sp,s->ly,s->mode,s->vram,s->palette,s->frontier);
}
static inline void pixel_access_print(const char *core, int complete, int enabled,
                                      const PixelAccessSample *samples, unsigned count,
                                      PixelAccessSample final, size_t state_size, uint64_t state_hash) {
    printf("{\"schema\":\"gbb-cgb-pixel-access-trace-v1\",\"qualification\":false,"
           "\"core\":\"%s\",\"model\":\"cgb-e\",\"clock_unit\":\"8mhz\","
           "\"anchor\":\"before_step_pc_1000\",\"complete\":%s,\"snapshots_enabled\":%s,"
           "\"state_bytes\":%zu,\"state_fnv1a64\":%" PRIu64 ",\"final\":",
           core,complete?"true":"false",enabled?"true":"false",state_size,state_hash);
    pixel_access_print_sample(&final);
    printf(",\"samples\":[");
    for (unsigned i=0;i<count;++i) { if(i) putchar(','); pixel_access_print_sample(&samples[i]); }
    puts("]}");
}
#endif
