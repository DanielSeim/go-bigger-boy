/* SPDX-License-Identifier: GPL-3.0-or-later
 * ROM BOOT [--end-only]; pinned SameBoy 1.0.3 internal ABI, read only.
 * Internal fields expose raw, ungated palette/VRAM without CPU accesses.
 */
#ifndef GB_INTERNAL
#error Build with GB_INTERNAL against pinned SameBoy
#endif
#include "Core/gb.h"
#include "pixel_access_trace.h"
#include <stdlib.h>
#include <string.h>
_Static_assert(STRUCT_VERSION==15,"Pinned SameBoy internal layout required");
static uint32_t pixels[160*144];
static void log_message(GB_gameboy_t *gb,const char *text,GB_log_attributes_t attributes) {
    (void)gb;(void)attributes;fputs(text,stderr);
}
static uint32_t rgb(GB_gameboy_t *gb,uint8_t r,uint8_t g,uint8_t b) {
    (void)gb;return (uint32_t)r<<16 | (uint32_t)g<<8 | b;
}
static PixelAccessSample snapshot(GB_gameboy_t *gb,uint64_t clock) {
    const unsigned ly=GB_read_memory(gb,0xff44); /* Flush deferred PPU output first. */
    const GB_registers_t *r=GB_get_registers(gb);
    return (PixelAccessSample){clock,r->pc,r->af,r->bc,r->de,r->hl,r->sp,ly,
        GB_read_memory(gb,0xff41)&3,gb->vram[0x100],gb->background_palettes_data[2],
        pixel_access_frontier(pixels,ly)};
}
int main(int argc,char **argv) {
    if(argc<3 || argc>4 || (argc==4 && strcmp(argv[3],"--end-only"))) return 2;
    GB_random_seed(0);GB_gameboy_t *gb=GB_alloc();if(!gb) return 3;
    if(!GB_init(gb,GB_MODEL_CGB_E)) {GB_dealloc(gb);return 3;}
    int result=3,active=0,complete=0,enabled=argc==3;
    uint64_t clocks=0,origin=0;
    PixelAccessSample samples[PIXEL_ACCESS_MAX_STEPS+1];
    unsigned count=0,steps=0,zeros=0;
    GB_set_log_callback(gb,log_message);GB_debugger_set_disabled(gb,true);
    GB_set_border_mode(gb,GB_BORDER_NEVER);GB_set_rgb_encode_callback(gb,rgb);
    GB_set_color_correction_mode(gb,GB_COLOR_CORRECTION_DISABLED);
    GB_set_pixels_output(gb,pixels);GB_set_turbo_mode(gb,true,true);GB_set_turbo_cap(gb,0);
    if(GB_load_rom(gb,argv[1]) || GB_load_boot_rom(gb,argv[2]) ||
       gb->rom_size!=32768 || gb->rom[0x147]!=0 || gb->rom[0x143]!=0xc0) goto done;
    while(clocks<PIXEL_ACCESS_CLOCK_LIMIT) {
        const unsigned pc=GB_get_registers(gb)->pc;
        if(!active && pc==0x1000) {active=1;origin=clocks;}
        if(active) {
            if(enabled) samples[count++]=snapshot(gb,clocks-origin);
            if(pc==0x1100) {complete=1;break;}
            if(steps++==PIXEL_ACCESS_MAX_STEPS) break;
        }
        unsigned step=GB_run(gb);clocks+=step;
        if(!step && ++zeros>1024) break;
        if(step) zeros=0;
    }
    PixelAccessSample final=snapshot(gb,clocks-origin);
    size_t size=GB_get_save_state_size(gb);
    if(!size || size>2*1024*1024) goto done;
    uint8_t *state=malloc(size);if(!state) goto done;
    GB_save_state_to_buffer(gb,state);
    /* Mask only the unused ROM-only host RTC epoch in this copied buffer,
     * as in sameboy_stat_clock_trace.c. Never alter the emulated state. */
    size_t epoch=GB_SECTION_SIZE(header)+7*sizeof(uint32_t)+GB_SECTION_SIZE(core_state)
        +GB_SECTION_SIZE(dma)+GB_SECTION_SIZE(mbc)+GB_SECTION_SIZE(hram)
        +GB_SECTION_SIZE(timing)+GB_SECTION_SIZE(apu)
        +offsetof(GB_gameboy_t,last_rtc_second)-GB_SECTION_OFFSET(rtc);
    if(epoch+sizeof(gb->last_rtc_second)>size) {free(state);goto done;}
    memset(state+epoch,0,sizeof(gb->last_rtc_second));
    pixel_access_print("sameboy",complete,enabled,samples,count,final,size,pixel_access_hash(state,size));
    free(state);result=complete?0:3;
done:
    GB_free(gb);GB_dealloc(gb);return result;
}
