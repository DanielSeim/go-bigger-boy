/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional public-Core-API reference adapter for pinned SameBoy v1.0.3.
 * ROM BOOT_ROM FRAMES OUTPUT.ppm [cgb-e|cgb-c|cgb0] [MAX_8MHZ_CLOCKS] [WRAM_OUTPUT]
 * Counts actual VBlanks; artificial LCD-off/repeat callbacks never advance N.
 */
#include "Core/gb.h"
#include "Core/display.h"
#include "Core/debugger.h"
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned frames, artificial;
static void log_message(GB_gameboy_t *gb, const char *text, GB_log_attributes_t attributes) {
    (void)gb; (void)attributes; fputs(text,stderr);
}
static void vblank(GB_gameboy_t *gb, GB_vblank_type_t type) {
    (void)gb;
    if (type == GB_VBLANK_TYPE_NORMAL_FRAME) ++frames;
    else ++artificial;
}
static uint32_t rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}
static int number(const char *text, uint64_t limit, uint64_t *value) {
    if (!*text || *text < '0' || *text > '9') return 0;
    char *end; errno=0;
    unsigned long long n=strtoull(text,&end,10);
    if (errno || *end || !n || n>limit) return 0;
    *value=n;return 1;
}
int main(int argc,char **argv) {
    uint64_t target,limit=200000000;
    if (argc<5 || argc>8 || !number(argv[3],100000,&target)) return 2;
    const char *model_name=argc>=6?argv[5]:"cgb-e";
    GB_model_t model;
    if (!strcmp(model_name,"cgb-e")) model=GB_MODEL_CGB_E;
    else if (!strcmp(model_name,"cgb-c")) model=GB_MODEL_CGB_C;
    else if (!strcmp(model_name,"cgb0")) model=GB_MODEL_CGB_0;
    else return 2;
    if (argc>=7 && !number(argv[6],UINT64_C(2000000000),&limit)) return 2;
    GB_random_seed(0);
    GB_gameboy_t *gb=GB_alloc();
    if (!gb) return 3;
    if (!GB_init(gb,model)) { GB_dealloc(gb);return 3; }
    GB_set_log_callback(gb,log_message);
    uint32_t pixels[160*144]={0};
    int result=4;
    if (GB_load_boot_rom(gb,argv[2]) || GB_load_rom(gb,argv[1])) goto done;
    GB_debugger_set_disabled(gb,true);
    GB_set_border_mode(gb,GB_BORDER_NEVER);
    GB_set_color_correction_mode(gb,GB_COLOR_CORRECTION_DISABLED);
    GB_set_rgb_encode_callback(gb,rgb);
    GB_set_pixels_output(gb,pixels);
    GB_set_vblank_callback(gb,vblank);
    GB_set_turbo_mode(gb,true,true);
    GB_set_turbo_cap(gb,0);
    if (GB_get_screen_width(gb)!=160 || GB_get_screen_height(gb)!=144) goto done;
    uint64_t clocks=0;
    unsigned zero_steps=0;
    while (frames<target && clocks<limit) {
        unsigned step=GB_run(gb);
        if (!step && ++zero_steps>1024) { result=3;goto done; }
        if (step) zero_steps=0;
        clocks+=step;
    }
    if (frames<target) {
        fprintf(stderr,"CGB capture clock budget exhausted before requested PPU frames\n");
        result=1;goto done;
    }
    FILE *out=fopen(argv[4],"wbx");
    if (!out) goto done;
    int ok=fprintf(out,"P6\n160 144\n255\n")>0;
    for (size_t i=0;ok && i<160*144;++i) {
        uint8_t p[]={(uint8_t)(pixels[i]>>16),(uint8_t)(pixels[i]>>8),(uint8_t)pixels[i]};
        ok=fwrite(p,1,3,out)==3;
    }
    if (fclose(out) || !ok) { remove(argv[4]);goto done; }
    if (argc==8) {
        FILE *memory=fopen(argv[7],"wbx");
        if (!memory) { remove(argv[4]);goto done; }
        ok=1;
        for (unsigned address=0xC000;ok && address<0xE000;++address)
            ok=fputc(GB_read_memory(gb,(uint16_t)address),memory)!=EOF;
        if (fclose(memory) || !ok) { remove(argv[7]);remove(argv[4]);goto done; }
    }
    printf("{\"schema\":\"gbb-sameboy-cgb-capture-v1\",\"qualification\":false,"
           "\"model\":\"%s\",\"completed_ppu_frames\":%u,\"artificial_callbacks\":%u,"
           "\"clocks_8mhz\":%" PRIu64 ",\"random_seed\":0,\"color_correction\":\"disabled\","
           "\"capture_anchor\":\"normal_vblank_including_boot\",\"pc\":%u,\"pcm12\":%u,\"pcm34\":%u}\n",
           model_name,frames,artificial,clocks,GB_get_registers(gb)->pc,
           GB_read_memory(gb,0xFF76),GB_read_memory(gb,0xFF77));
    result=0;
done:
    GB_free(gb);GB_dealloc(gb);return result;
}
