/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional execution-only AGB handoff capture through SameBoy's public API.
 * Usage: capture USER_CARTRIDGE USER_BOOT_ROM
 * Only the cartridge header is read; neither firmware instructions nor logo
 * assets are decoded or emitted. Header variants exist only in memory.
 */
#include "Core/gb.h"
#include "Core/debugger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int done;
static GB_registers_t handoff;
static void log_message(GB_gameboy_t *gb, const char *message, GB_log_attributes_t attributes) {
    (void)gb; (void)attributes;
    fputs(message, stderr);
}
static uint32_t rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}
static void execution(GB_gameboy_t *gb, uint16_t address, uint8_t opcode) {
    (void)opcode;
    if (address == 0x100 && !done) {
        handoff = *GB_get_registers(gb);
        handoff.pc = address; /* callback is after opcode fetch, before execution */
        done = 1;
    }
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    uint8_t header[0x150];
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 3;
    const size_t count = fread(header, 1, sizeof header, input);
    fclose(input);
    if (count != sizeof header) return 3;
    puts("color,license,title,af,bc,de,hl,sp,pc");
    for (unsigned color = 0; color < 2; ++color)
    for (unsigned license = 0; license < 3; ++license)
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned titles[] = {0, 0x0f, 0x12, 0x43, 0x58, 0xff};
        uint8_t cartridge[32768] = {0};
        memcpy(cartridge, header, sizeof header);
        memset(cartridge + 0x134, 0, 0x14d - 0x134);
        cartridge[0x134] = titles[i];
        cartridge[0x143] = color ? 0x80 : 0;
        cartridge[0x14b] = license == 2 ? 0x33 : license;
        if (license == 2) { cartridge[0x144] = '0'; cartridge[0x145] = '1'; }
        cartridge[0x14d] = 0;
        for (unsigned address = 0x134; address <= 0x14c; ++address)
            cartridge[0x14d] -= cartridge[address] + 1;
        GB_gameboy_t *gb = GB_init(GB_alloc(), GB_MODEL_AGB);
        if (!gb) return 4;
        GB_set_log_callback(gb, log_message);
        GB_debugger_set_disabled(gb, true);
        GB_set_turbo_mode(gb, true, true);
        GB_set_turbo_cap(gb, 0);
        uint32_t pixels[160 * 144];
        GB_set_pixels_output(gb, pixels);
        GB_set_rgb_encode_callback(gb, rgb);
        GB_load_rom_from_buffer(gb, cartridge, sizeof cartridge);
        if (GB_load_boot_rom(gb, argv[2])) { GB_free(gb); free(gb); return 4; }
        done = 0;
        GB_set_execution_callback(gb, execution);
        uint64_t clocks = 0;
        while (!done && clocks < 80000000) clocks += GB_run(gb);
        if (!done) {
            fprintf(stderr, "No handoff: model AGB, color %u license %u title %02x, PC %04x after %llu clocks\n",
                    color, license, titles[i], GB_get_registers(gb)->pc, (unsigned long long)clocks);
            GB_free(gb); free(gb);
            return 5;
        }
        GB_free(gb); free(gb);
        printf("%u,%u,%02x,%04x,%04x,%04x,%04x,%04x,%04x\n", color, license,
               titles[i], handoff.af, handoff.bc, handoff.de, handoff.hl, handoff.sp, handoff.pc);
    }
}
