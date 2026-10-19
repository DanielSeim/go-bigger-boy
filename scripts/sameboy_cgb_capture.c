/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional public-Core-API reference adapter for pinned SameBoy v1.0.3.
 * ROM BOOT_ROM FRAMES OUTPUT.ppm [cgb-e|cgb-c|cgb0] [MAX_8MHZ_CLOCKS] [WRAM_OUTPUT]
 * Counts actual VBlanks; artificial LCD-off/repeat callbacks never advance N.
 * Or: --mooneye ROM BOOT_ROM [cgb-e|cgb-c|cgb0] [MAX_8MHZ_CLOCKS]
 * Or: --pc-clock-window ROM BOOT_ROM START_PC END_PC [MODEL] [MAX_8MHZ_CLOCKS]
 * Or: --boot-registers ROM BOOT_ROM [MODEL] [MAX_8MHZ_CLOCKS]
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
static int model_from_name(const char *name, GB_model_t *model) {
    if (!strcmp(name,"cgb-e")) *model=GB_MODEL_CGB_E;
    else if (!strcmp(name,"cgb-c")) *model=GB_MODEL_CGB_C;
    else if (!strcmp(name,"cgb0")) *model=GB_MODEL_CGB_0;
    else return 0;
    return 1;
}
static int protocol_result=-1;
static int boot_registers(int argc, char **argv) {
    if (argc<4 || argc>6) return 2;
    const char *name=argc>=5?argv[4]:"cgb-e";
    GB_model_t model;
    uint64_t limit=200000000;
    if (!model_from_name(name,&model) ||
        (argc==6 && !number(argv[5],UINT64_C(2000000000),&limit))) return 2;
    GB_random_seed(0);
    GB_gameboy_t *gb=GB_alloc();
    if (!gb) return 3;
    if (!GB_init(gb,model)) { GB_dealloc(gb);return 3; }
    int result=4;
    uint32_t pixels[160*144]={0};
    GB_set_log_callback(gb,log_message);
    if (GB_load_boot_rom(gb,argv[3]) || GB_load_rom(gb,argv[2])) goto done;
    GB_debugger_set_disabled(gb,true);
    GB_set_border_mode(gb,GB_BORDER_NEVER);
    GB_set_rgb_encode_callback(gb,rgb);
    GB_set_pixels_output(gb,pixels);
    GB_set_turbo_mode(gb,true,true);
    GB_set_turbo_cap(gb,0);
    uint64_t clocks=0;
    unsigned zero_steps=0;
    int handoff=0;
    for (;;) {
        if ((GB_read_memory(gb,0xFF50)&1) && GB_get_registers(gb)->pc==0x100) {
            handoff=1;break;
        }
        if (clocks>=limit) break;
        unsigned step=GB_run(gb);
        if (!step && ++zero_steps>1024) { result=3;goto done; }
        if (step) zero_steps=0;
        clocks+=step;
    }
    const GB_registers_t *r=GB_get_registers(gb);
    printf("{\"schema\":\"gbb-sameboy-cgb-boot-registers-v1\",\"qualification\":false,"
           "\"model\":\"%s\",\"result\":\"%s\","
           "\"anchor\":\"boot_unmapped_before_cartridge_entry\","
           "\"clocks_8mhz\":%" PRIu64 ",\"pc\":%u,\"sp\":%u,"
           "\"af\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u}\n",
           name,handoff?"handoff":"timeout",clocks,r->pc,r->sp,
           r->af,r->bc,r->de,r->hl);
    result=handoff?0:3;
done:
    GB_free(gb);GB_dealloc(gb);return result;
}
static void mooneye_instruction(GB_gameboy_t *gb, uint16_t address, uint8_t opcode) {
    (void)address;
    if (opcode!=0x40 || protocol_result!=-1) return;
    const GB_registers_t *r=GB_get_registers(gb);
    if (r->bc==0x0305 && r->de==0x080d && r->hl==0x1522) protocol_result=0;
    else if (r->bc==0x4242 && r->de==0x4242 && r->hl==0x4242) protocol_result=1;
}
static int mooneye(int argc, char **argv) {
    if (argc<4 || argc>6) return 2;
    const char *name=argc>=5?argv[4]:"cgb-e";
    GB_model_t model;
    uint64_t limit=200000000;
    if (!model_from_name(name,&model) ||
        (argc==6 && !number(argv[5],UINT64_C(2000000000),&limit))) return 2;
    GB_random_seed(0);
    GB_gameboy_t *gb=GB_alloc();
    if (!gb) return 3;
    if (!GB_init(gb,model)) { GB_dealloc(gb);return 3; }
    GB_set_log_callback(gb,log_message);
    int result=4;
    uint32_t pixels[160*144]={0};
    if (GB_load_boot_rom(gb,argv[3]) || GB_load_rom(gb,argv[2])) goto done;
    GB_debugger_set_disabled(gb,true);
    GB_set_border_mode(gb,GB_BORDER_NEVER);
    GB_set_color_correction_mode(gb,GB_COLOR_CORRECTION_DISABLED);
    GB_set_rgb_encode_callback(gb,rgb);
    GB_set_pixels_output(gb,pixels);
    GB_set_turbo_mode(gb,true,true);
    GB_set_turbo_cap(gb,0);
    GB_set_execution_callback(gb,mooneye_instruction);
    uint64_t clocks=0;
    unsigned zero_steps=0;
    while (protocol_result==-1 && clocks<limit) {
        unsigned step=GB_run(gb);
        if (!step && ++zero_steps>1024) { result=3;goto done; }
        if (step) zero_steps=0;
        clocks+=step;
    }
    const GB_registers_t *r=GB_get_registers(gb);
    printf("{\"schema\":\"gbb-sameboy-mooneye-v1\",\"model\":\"%s\","
           "\"result\":\"%s\",\"clocks_8mhz\":%" PRIu64 ","
           "\"pc\":%u,\"bc\":%u,\"de\":%u,\"hl\":%u}\n",
           name,protocol_result==0?"pass":protocol_result==1?"fail":"timeout",
           clocks,r->pc,r->bc,r->de,r->hl);
    result=protocol_result==-1?3:protocol_result;
done:
    GB_free(gb);GB_dealloc(gb);return result;
}

static uint64_t window_clocks, window_entry, window_exit;
static uint16_t window_start_pc, window_end_pc;
static unsigned window_entry_opcode, window_exit_opcode;
static unsigned window_entry_io[4], window_exit_io[4];
static unsigned window_entry_bc, window_exit_bc, window_records;
static uint64_t window_callback_count;
static struct { uint16_t pc,bc; uint8_t opcode; uint64_t clocks; } window_trace[8];
static int window_started, window_finished;
static void window_instruction(GB_gameboy_t *gb, uint16_t address, uint8_t opcode) {
    const int already_finished=window_finished;
    unsigned *io=NULL;
    if (!window_started && address==window_start_pc) {
        window_started=1;window_entry=window_clocks;window_entry_opcode=opcode;
        window_entry_bc=GB_get_registers(gb)->bc;
        io=window_entry_io;
    }
    else if (window_started && !window_finished && address==window_end_pc) {
        window_finished=1;window_exit=window_clocks;window_exit_opcode=opcode;
        window_exit_bc=GB_get_registers(gb)->bc;
        io=window_exit_io;
    }
    if (io) {
        io[0]=GB_read_memory(gb,0xFF4D);io[1]=GB_read_memory(gb,0xFF04);
        io[2]=GB_read_memory(gb,0xFF05);io[3]=GB_read_memory(gb,0xFF76);
    }
    if (window_started && !already_finished) {
        ++window_callback_count;
        if (window_records<8) {
            window_trace[window_records].pc=address;
            window_trace[window_records].opcode=opcode;
            window_trace[window_records].clocks=window_clocks;
            window_trace[window_records].bc=GB_get_registers(gb)->bc;
            ++window_records;
        }
    }
}
static int pc_number(const char *text, uint16_t *value) {
    if (!*text || *text<'0' || *text>'9') return 0;
    char *end;errno=0;
    unsigned long n=strtoul(text,&end,0);
    if (errno || *end || n>65535) return 0;
    *value=(uint16_t)n;return 1;
}
static int pc_clock_window(int argc, char **argv) {
    if (argc<6 || argc>8 || !pc_number(argv[4],&window_start_pc) ||
        !pc_number(argv[5],&window_end_pc) || window_start_pc==window_end_pc) return 2;
    const char *name=argc>=7?argv[6]:"cgb-e";
    GB_model_t model;
    uint64_t limit=20000000;
    if (!model_from_name(name,&model) ||
        (argc==8 && !number(argv[7],UINT64_C(2000000000),&limit))) return 2;
    GB_random_seed(0);
    GB_gameboy_t *gb=GB_alloc();
    if (!gb) return 3;
    if (!GB_init(gb,model)) { GB_dealloc(gb);return 3; }
    GB_set_log_callback(gb,log_message);
    int result=4;
    uint32_t pixels[160*144]={0};
    if (GB_load_boot_rom(gb,argv[3]) || GB_load_rom(gb,argv[2])) goto done;
    GB_debugger_set_disabled(gb,true);
    GB_set_border_mode(gb,GB_BORDER_NEVER);
    GB_set_color_correction_mode(gb,GB_COLOR_CORRECTION_DISABLED);
    GB_set_rgb_encode_callback(gb,rgb);
    GB_set_pixels_output(gb,pixels);
    GB_set_turbo_mode(gb,true,true);
    GB_set_turbo_cap(gb,0);
    GB_set_execution_callback(gb,window_instruction);
    unsigned zero_steps=0;
    while (!window_finished && window_clocks<limit) {
        unsigned step=GB_run(gb);
        if (!step && ++zero_steps>1024) { result=3;goto done; }
        if (step) zero_steps=0;
        window_clocks+=step;
    }
    printf("{\"schema\":\"gbb-sameboy-pc-clock-window-v1\",\"qualification\":false,"
           "\"model\":\"%s\",\"result\":\"%s\","
           "\"anchor\":\"gb_run_call_before_instruction_callback\","
           "\"start_pc\":%u,\"end_pc\":%u,\"entry_seen\":%s,\"exit_seen\":%s,"
           "\"entry_8mhz_clocks\":%" PRIu64 ",\"exit_8mhz_clocks\":%" PRIu64 ","
           "\"elapsed_8mhz_clocks\":%" PRIu64 ",\"entry_opcode\":%u,\"exit_opcode\":%u,"
           "\"entry_key1\":%u,\"exit_key1\":%u,\"entry_div\":%u,\"exit_div\":%u,"
           "\"entry_tima\":%u,\"exit_tima\":%u,\"entry_pcm12\":%u,\"exit_pcm12\":%u,"
           "\"entry_bc\":%u,\"exit_bc\":%u,\"instruction_callbacks\":%" PRIu64 ","
           "\"callback_trace_truncated\":%s,\"callback_trace\":[",
           name,window_finished?"complete":"timeout",window_start_pc,window_end_pc,
           window_started?"true":"false",window_finished?"true":"false",window_entry,window_exit,
           window_finished?window_exit-window_entry:0,window_entry_opcode,window_exit_opcode,
           window_entry_io[0],window_exit_io[0],window_entry_io[1],window_exit_io[1],
           window_entry_io[2],window_exit_io[2],window_entry_io[3],window_exit_io[3],
           window_entry_bc,window_exit_bc,window_callback_count,window_callback_count>8?"true":"false");
    for (unsigned i=0;i<window_records;++i) {
        printf("%s{\"pc\":%u,\"opcode\":%u,\"clocks_8mhz\":%" PRIu64 ",\"bc\":%u}",
               i?",":"",window_trace[i].pc,window_trace[i].opcode,window_trace[i].clocks,window_trace[i].bc);
    }
    puts("]}");
    result=window_finished?0:3;
done:
    GB_free(gb);GB_dealloc(gb);return result;
}
int main(int argc,char **argv) {
    if (argc>=2 && !strcmp(argv[1],"--boot-registers")) return boot_registers(argc,argv);
    if (argc>=2 && !strcmp(argv[1],"--mooneye")) return mooneye(argc,argv);
    if (argc>=2 && !strcmp(argv[1],"--pc-clock-window")) return pc_clock_window(argc,argv);
    uint64_t target,limit=200000000;
    if (argc<5 || argc>8 || !number(argv[3],100000,&target)) return 2;
    const char *model_name=argc>=6?argv[5]:"cgb-e";
    GB_model_t model;
    if (!model_from_name(model_name,&model)) return 2;
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
