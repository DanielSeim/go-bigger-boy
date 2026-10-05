/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional execution-only SameBoy probe. No reference code/data is decoded,
 * exported or included in the GBB firmware build. Usage: MODEL GAME BOOT.
 */
#include "Core/gb.h"
#include "Core/debugger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int done;
static GB_registers_t handoff;
static unsigned ly, divider, stat;
static void log_message(GB_gameboy_t *gb, const char *message, GB_log_attributes_t attributes) {
    (void)gb; (void)attributes; fputs(message,stderr);
}
static uint32_t rgb(GB_gameboy_t *gb,uint8_t r,uint8_t g,uint8_t b) {
    (void)gb; return (uint32_t)r<<16 | (uint32_t)g<<8 | b;
}
static void execution(GB_gameboy_t *gb,uint16_t address,uint8_t opcode) {
    (void)opcode;
    if(address==0x100 && !done) {
        handoff=*GB_get_registers(gb); handoff.pc=address;
        ly=GB_safe_read_memory(gb,0xff44); divider=GB_safe_read_memory(gb,0xff04);
        stat=GB_safe_read_memory(gb,0xff41); done=1;
    }
}
int main(int argc,char **argv) {
    if(argc!=4 || (strcmp(argv[1],"sgb") && strcmp(argv[1],"sgb2"))) return 2;
    GB_gameboy_t *gb=GB_init(GB_alloc(),!strcmp(argv[1],"sgb2") ? GB_MODEL_SGB2 : GB_MODEL_SGB);
    if(!gb) return 3;
    GB_set_log_callback(gb,log_message); GB_debugger_set_disabled(gb,true);
    GB_set_turbo_mode(gb,true,true); GB_set_turbo_cap(gb,0);
    uint32_t pixels[256*224]; GB_set_pixels_output(gb,pixels); GB_set_rgb_encode_callback(gb,rgb);
    if(GB_load_rom(gb,argv[2]) || GB_load_boot_rom(gb,argv[3])) { GB_free(gb); free(gb); return 4; }
    GB_set_execution_callback(gb,execution);
    uint64_t clocks=0;
    while(!done && clocks<20000000) clocks+=GB_run(gb);
    if(done) printf("{\"af\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u,\"sp\":%u,\"pc\":%u,\"ly\":%u,\"div\":%u,\"stat\":%u}\n",
        handoff.af,handoff.bc,handoff.de,handoff.hl,handoff.sp,handoff.pc,ly,divider,stat);
    GB_free(gb); free(gb); return done ? 0 : 5;
}
