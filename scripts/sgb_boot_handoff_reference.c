/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional execution-only SameBoy probe. No reference code/data is decoded,
 * exported or included in the GBB firmware build. Usage: MODEL GAME BOOT.
 */
#define GB_INTERNAL
#include "Core/gb.h"
#include "Core/debugger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int done;
static GB_registers_t handoff;
static unsigned ly, divider, stat;
static uint64_t clocks;
static unsigned divider_counter;
static unsigned apu_phase[20];
static unsigned ppu_phase[4];
static unsigned trace;
static unsigned trace_count;
static struct { uint64_t cycle; unsigned address, value; } writes[4096];
static struct { uint64_t first, last; unsigned value, count; } reads[8192];
static unsigned read_count;
static uint8_t ly_read(GB_gameboy_t *gb, uint16_t address, uint8_t value) {
    if (trace && address==0xff44) {
        uint64_t cycle=(clocks+gb->cycles_since_run)/2;
        if (read_count && reads[read_count-1].value==value) {
            reads[read_count-1].last=cycle; ++reads[read_count-1].count;
        } else if (read_count<8192) {
            reads[read_count].first=reads[read_count].last=cycle;
            reads[read_count].value=value; reads[read_count++].count=1;
        }
    }
    return value;
}
static bool io_write(GB_gameboy_t *gb, uint16_t address, uint8_t value) {
    if (trace && address >= 0xff00 && address <= 0xff7f && trace_count < 4096) {
        writes[trace_count].cycle = (clocks + gb->cycles_since_run) / 2;
        writes[trace_count].address = address;
        writes[trace_count++].value = value;
    }
    return true;
}
static void log_message(GB_gameboy_t *gb, const char *message, GB_log_attributes_t attributes) {
    (void)gb; (void)attributes; fputs(message,stderr);
}
static uint32_t rgb(GB_gameboy_t *gb,uint8_t r,uint8_t g,uint8_t b) {
    (void)gb; return (uint32_t)r<<16 | (uint32_t)g<<8 | b;
}
static void audio(GB_gameboy_t *gb, GB_sample_t *sample) { (void)gb; (void)sample; }
static void execution(GB_gameboy_t *gb,uint16_t address,uint8_t opcode) {
    (void)opcode;
    if(address==0x100 && !done) {
        handoff=*GB_get_registers(gb); handoff.pc=address;
        ly=GB_safe_read_memory(gb,0xff44); divider=GB_safe_read_memory(gb,0xff04);
        stat=GB_safe_read_memory(gb,0xff41); done=1;
        divider_counter=gb->div_counter;
        const unsigned phase[]={gb->apu.apu_cycles,gb->apu.div_divider,gb->apu.lf_div,
            gb->apu.skip_div_event,gb->apu.global_enable,
            gb->apu.square_channels[0].sample_countdown,gb->apu.square_channels[0].current_sample_index,
            gb->apu.square_channels[0].pulse_length,gb->apu.square_channels[0].current_volume,
            gb->apu.square_channels[0].volume_countdown,
            gb->apu.square_channels[1].sample_countdown,gb->apu.square_channels[1].current_sample_index,
            gb->apu.square_channels[1].pulse_length,gb->apu.square_channels[1].current_volume,
            gb->apu.square_channels[1].volume_countdown,
            gb->apu.wave_channel.sample_countdown,gb->apu.wave_channel.current_sample_index,
            gb->apu.noise_channel.lfsr,gb->apu_output.sample_cycles,gb->apu_output.sample_fraction};
        memcpy(apu_phase,phase,sizeof phase);
        ppu_phase[0]=gb->current_line; ppu_phase[1]=gb->display_state;
        ppu_phase[2]=gb->display_cycles; ppu_phase[3]=gb->position_in_line;
    }
}
int main(int argc,char **argv) {
    if((argc!=4 && argc!=5) || (strcmp(argv[1],"sgb") && strcmp(argv[1],"sgb2"))) return 2;
    if (argc==5) { if (strcmp(argv[4], "--trace")) return 2; trace=1; }
    /* Validate the GB-side chip without an unrelated HLE SNES intro/checker. */
    GB_gameboy_t *gb=GB_init(GB_alloc(),!strcmp(argv[1],"sgb2") ? GB_MODEL_SGB2_NO_SFC : GB_MODEL_SGB_NO_SFC);
    if(!gb) return 3;
    GB_set_log_callback(gb,log_message); GB_debugger_set_disabled(gb,true);
    GB_set_turbo_mode(gb,true,true); GB_set_turbo_cap(gb,0);
    GB_set_sample_rate(gb,48000);
    GB_apu_set_sample_callback(gb,audio);
    uint32_t pixels[256*224]; GB_set_pixels_output(gb,pixels); GB_set_rgb_encode_callback(gb,rgb);
    if(GB_load_rom(gb,argv[2]) || GB_load_boot_rom(gb,argv[3])) { GB_free(gb); free(gb); return 4; }
    GB_set_execution_callback(gb,execution);
    GB_set_write_memory_callback(gb,io_write);
    GB_set_read_memory_callback(gb,ly_read);
    while(!done && clocks<20000000) clocks+=GB_run(gb);
    if(done) {
        printf("{\"af\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,\"sp\":%u,\"pc\":%u,\"ly\":%u,\"div\":%u,\"stat\":%u,\"divider_counter\":%u,\"writes\":[",
            handoff.af,handoff.bc,handoff.de,handoff.hl,handoff.sp,handoff.pc,ly,divider,stat,divider_counter);
        for (unsigned i=0;i<trace_count;++i) printf("%s[%llu,%u,%u]", i?",":"",
            (unsigned long long)writes[i].cycle,writes[i].address,writes[i].value);
        printf("],\"apu_phase\":[");
        for (unsigned i=0;i<20;++i) printf("%s%u",i?",":"",apu_phase[i]);
        printf("],\"ppu_phase\":[");
        for (unsigned i=0;i<4;++i) printf("%s%u",i?",":"",ppu_phase[i]);
        printf("],\"ly_reads\":[");
        for (unsigned i=0;i<read_count;++i) printf("%s[%llu,%llu,%u,%u]",i?",":"",
            (unsigned long long)reads[i].first,(unsigned long long)reads[i].last,reads[i].value,reads[i].count);
        puts("]}");
    }
    GB_free(gb); free(gb); return done ? 0 : 5;
}
