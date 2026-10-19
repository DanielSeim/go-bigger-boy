/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional pinned SameBoy 1.0.3 INTERNAL-ABI observer, not the public adapter.
 * Build with -DGB_INTERNAL against the exact pinned Core and library.
 * cycles_since_run is read only, to timestamp callbacks within GB_run.
 */
#ifndef GB_INTERNAL
#error Build this optional observer with -DGB_INTERNAL against pinned SameBoy
#endif
#include "Core/gb.h"
#include "stat_clock_trace.h"
#include <string.h>
#include <stdlib.h>
_Static_assert(STRUCT_VERSION == 15, "Pinned SameBoy internal layout required");
static void log_message(GB_gameboy_t *gb, const char *text, GB_log_attributes_t attributes) {
    (void)gb; (void)attributes; fputs(text,stderr);
}
static StatClockTrace trace;
static uint64_t clocks;
static uint64_t now(const GB_gameboy_t *gb) { return clocks+gb->cycles_since_run; }
static void observe(GB_gameboy_t *gb) {
    stat_clock_observe(&trace,now(gb),gb->io_registers[GB_IO_IF]);
}
static uint8_t read_memory(GB_gameboy_t *gb, uint16_t address, uint8_t data) {
    (void)address; observe(gb); return data;
}
static bool write_memory(GB_gameboy_t *gb, uint16_t address, uint8_t data) {
    (void)address; (void)data; observe(gb); return true;
}
static void lcd(GB_gameboy_t *gb, bool on) { if (on) stat_clock_lcd(&trace,now(gb)); }
static void instruction(GB_gameboy_t *gb, uint16_t pc, uint8_t opcode) {
    (void)opcode; observe(gb); stat_clock_vector(&trace,now(gb),pc);
}
static uint32_t rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb; return (uint32_t)r<<16 | (uint32_t)g<<8 | b;
}
int main(int argc, char **argv) {
    if (argc<3 || argc>4 || (argc==4 && strcmp(argv[3],"--no-observers"))) return 2;
    GB_random_seed(0);
    GB_gameboy_t *gb=GB_alloc();
    if (!gb) return 3;
    if (!GB_init(gb,GB_MODEL_CGB_E)) { GB_dealloc(gb); return 3; }
    int result=3, complete=0, enabled=argc==3;
    GB_set_log_callback(gb,log_message);
    uint32_t pixels[160*144]={0};
    if (GB_load_boot_rom(gb,argv[2]) || GB_load_rom(gb,argv[1])) goto done;
    /* This diagnostic is restricted to authored 32-KiB ROM-only cartridges.
     * Their RTC state is never clocked; only its unused host epoch varies. */
    if (gb->rom_size!=32768 || gb->rom[0x147]!=0) goto done;
    GB_debugger_set_disabled(gb,true);
    GB_set_border_mode(gb,GB_BORDER_NEVER);
    GB_set_rgb_encode_callback(gb,rgb);
    GB_set_pixels_output(gb,pixels);
    GB_set_turbo_mode(gb,true,true);
    GB_set_turbo_cap(gb,0);
    if (enabled) {
        GB_set_read_memory_callback(gb,read_memory);
        GB_set_write_memory_callback(gb,write_memory);
        GB_set_lcd_status_callback(gb,lcd);
        GB_set_execution_callback(gb,instruction);
    }
    unsigned zero_steps=0;
    while (clocks<STAT_CLOCK_LIMIT && !trace.overflow) {
        const GB_registers_t before=*GB_get_registers(gb);
        if (!trace.active && before.pc==0x1000) { trace.active=1; trace.origin=clocks; }
        if (trace.active && before.pc==0x1100) { complete=1; break; }
        const unsigned flags=gb->io_registers[GB_IO_IF]&31;
        const unsigned pending=flags&gb->interrupt_enable;
        const bool ime=gb->ime;
        stat_clock_observe(&trace,clocks,flags);
        const uint64_t begin=clocks;
        unsigned step=GB_run(gb);
        if (!step && ++zero_steps>1024) goto done;
        if (step) zero_steps=0;
        clocks+=step;
        const GB_registers_t *after=GB_get_registers(gb);
        unsigned next_flags=gb->io_registers[GB_IO_IF]&31;
        stat_clock_observe(&trace,clocks,next_flags);
        stat_clock_step(&trace,(StatClockStep){begin,clocks,before.pc,after->pc,
                                             before.bc,after->bc,flags,next_flags});
        /* In these armed, unhalted authored foregrounds the pinned Core
         * samples its interrupt queue before advancing this GB_run call.
         * Confirm actual entry before recording the pre-call sample. */
        if (enabled && ime && pending && after->pc==0x48 && before.pc!=0x48)
            stat_clock_sample(&trace,begin,before.pc,pending,0);
    }
    const GB_registers_t *r=GB_get_registers(gb);
    const uint16_t regs[]={r->pc,r->sp,r->af,r->bc,r->de,r->hl};
    uint8_t wram[64];
    /* No diagnostic reads while callbacks remain installed. */
    GB_set_read_memory_callback(gb,NULL);
    for (unsigned i=0;i<64;++i) wram[i]=GB_read_memory(gb,(uint16_t)(0xc000+i));
    size_t state_size=GB_get_save_state_size(gb);
    if (!state_size || state_size>2*1024*1024) goto done;
    uint8_t *state=malloc(state_size);
    if (!state) goto done;
    GB_save_state_to_buffer(gb,state);
    /* Match the pinned serializer's header and six preceding sized sections.
     * Normalize ONLY this copied buffer, never gb->last_rtc_second itself. */
    size_t rtc_timestamp=GB_SECTION_SIZE(header)+7*sizeof(uint32_t)
        +GB_SECTION_SIZE(core_state)+GB_SECTION_SIZE(dma)+GB_SECTION_SIZE(mbc)
        +GB_SECTION_SIZE(hram)+GB_SECTION_SIZE(timing)+GB_SECTION_SIZE(apu)
        +offsetof(GB_gameboy_t,last_rtc_second)-GB_SECTION_OFFSET(rtc);
    if (rtc_timestamp+sizeof(gb->last_rtc_second)>state_size) { free(state); goto done; }
    memset(state+rtc_timestamp,0,sizeof(gb->last_rtc_second));
    uint64_t state_hash=stat_clock_hash(state,state_size);
    free(state);
    stat_clock_print(&trace,"sameboy","before_opcode_fetch_pending_cycle",enabled,complete,
                     clocks,regs,wram,state_size,state_hash,sizeof(gb->last_rtc_second));
    result=complete && !trace.overflow?0:3;
done:
    GB_free(gb); GB_dealloc(gb); return result;
}
