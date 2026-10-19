/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional pinned SameBoy 1.0.3 public-API elapsed-clock reference adapter.
 * ROM BOOT_ROM cgb-e|sgb TARGETS_8MHZ OUTPUT_PREFIX
 * Targets are positive, increasing comma-separated clocks after boot unmap
 * at PC=0100. Pixel output is synchronized to already elapsed PPU time.
 */
#include "Core/gb.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_TARGETS 8
#define CLOCK_LIMIT UINT64_C(200000000)
static unsigned frames, artificial;
static void vblank(GB_gameboy_t *gb, GB_vblank_type_t type) {
    (void)gb;
    if (type==GB_VBLANK_TYPE_NORMAL_FRAME) ++frames;
    else ++artificial;
}
static uint32_t rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb; return (uint32_t)r<<16 | (uint32_t)g<<8 | b;
}
static void log_message(GB_gameboy_t *gb, const char *message, GB_log_attributes_t attributes) {
    (void)gb; (void)attributes; fputs(message,stderr);
}
static unsigned targets_from_text(const char *cursor, uint64_t targets[MAX_TARGETS]) {
    unsigned count=0;
    while (*cursor) {
        if (*cursor<'0' || *cursor>'9' || count==MAX_TARGETS) return 0;
        char *end; errno=0;
        unsigned long long value=strtoull(cursor,&end,10);
        if (errno || !value || value>CLOCK_LIMIT || (*end && *end!=',') ||
            (count && value<=targets[count-1])) return 0;
        targets[count++]=value;
        if (!*end) break;
        cursor=end+1;
        if (!*cursor) return 0;
    }
    return count;
}
int main(int argc, char **argv) {
    if (argc!=6) return 2;
    GB_model_t model;
    if (!strcmp(argv[3],"cgb-e")) model=GB_MODEL_CGB_E;
    else if (!strcmp(argv[3],"sgb")) model=GB_MODEL_SGB_NTSC;
    else return 2;
    uint64_t targets[MAX_TARGETS];
    unsigned count=targets_from_text(argv[4],targets);
    if (!count) return 2;
    GB_random_seed(0);
    GB_gameboy_t *gb=GB_alloc();
    if (!gb) return 3;
    if (!GB_init(gb,model)) { GB_dealloc(gb); return 3; }
    int result=4;
    uint32_t pixels[160*144]={0}, before_sync[160*144];
    GB_set_log_callback(gb,log_message);
    GB_debugger_set_disabled(gb,true);
    GB_set_border_mode(gb,GB_BORDER_NEVER);
    GB_set_color_correction_mode(gb,GB_COLOR_CORRECTION_DISABLED);
    GB_set_rgb_encode_callback(gb,rgb);
    GB_set_pixels_output(gb,pixels);
    GB_set_vblank_callback(gb,vblank);
    GB_set_turbo_mode(gb,true,true);
    GB_set_turbo_cap(gb,0);
    if (GB_load_rom(gb,argv[1]) || GB_load_boot_rom(gb,argv[2]) ||
        GB_get_screen_width(gb)!=160 || GB_get_screen_height(gb)!=144) goto done;
    uint64_t boot_clocks=0;
    unsigned zeros=0;
    while (!(GB_read_memory(gb,0xff50)&1) || GB_get_registers(gb)->pc!=0x100) {
        unsigned step=GB_run(gb);
        boot_clocks+=step;
        if ((!step && ++zeros>1024) || boot_clocks>CLOCK_LIMIT) goto done;
        if (step) zeros=0;
    }
    const GB_registers_t *regs=GB_get_registers(gb);
    printf("{\"event\":\"handoff\",\"model\":\"%s\",\"boot_clocks_8mhz\":%" PRIu64
           ",\"pc\":%u,\"af\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,\"sp\":%u,"
           "\"div\":%u,\"lcdc\":%u,\"ly\":%u}\n",argv[3],boot_clocks,regs->pc,
           regs->af,regs->bc,regs->de,regs->hl,regs->sp,
           GB_read_memory(gb,0xff04),GB_read_memory(gb,0xff40),GB_read_memory(gb,0xff44));
    frames=artificial=0;
    uint64_t clocks=0;
    for (unsigned t=0;t<count;++t) {
        while (clocks<targets[t]) {
            unsigned step=GB_run(gb);
            clocks+=step;
            if (!step && ++zeros>1024) goto done;
            if (step) zeros=0;
        }
        memcpy(before_sync,pixels,sizeof(pixels));
        /* GB_run can leave deferred pixel work. A public LY read synchronizes
         * it without executing another instruction or adding CPU clocks.
         * Perform this BEFORE consuming pixels, rather than only in metadata. */
        const unsigned ly=GB_read_memory(gb,0xff44);
        unsigned flushed=0;
        for (unsigned i=0;i<160*144;++i) flushed+=before_sync[i]!=pixels[i];
        char path[4096];
        if (snprintf(path,sizeof(path),"%s-%" PRIu64 ".ppm",argv[5],targets[t])>=(int)sizeof(path)) goto done;
        FILE *file=fopen(path,"wbx");
        if (!file) goto done;
        int ok=fprintf(file,"P6\n160 144\n255\n")>0;
        for (unsigned i=0;ok && i<160*144;++i) {
            uint8_t pixel[]={(uint8_t)(pixels[i]>>16),(uint8_t)(pixels[i]>>8),(uint8_t)pixels[i]};
            ok=fwrite(pixel,1,3,file)==3;
        }
        if (fclose(file) || !ok) { remove(path); goto done; }
        printf("{\"event\":\"capture\",\"schema\":\"gbb-sameboy-elapsed-capture-v1\","
               "\"qualification\":false,\"anchor\":\"elapsed_8mhz_after_boot_unmap_pc_0100\","
               "\"ppu_synchronized\":true,\"pixels_flushed\":%u,"
               "\"target_8mhz\":%" PRIu64 ",\"clocks_8mhz\":%" PRIu64
               ",\"completed_frames\":%u,\"artificial_callbacks\":%u,\"pc\":%u,\"ly\":%u,\"lcdc\":%u}\n",
               flushed,targets[t],clocks,frames,artificial,GB_get_registers(gb)->pc,ly,GB_read_memory(gb,0xff40));
    }
    result=0;
done:
    GB_free(gb); GB_dealloc(gb); return result;
}
