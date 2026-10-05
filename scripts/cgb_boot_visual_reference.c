/* SPDX-License-Identifier: GPL-3.0-or-later
 * Optional black-box reference capture using SameBoy's public Core API.
 * No firmware code, decoded glyphs, or reference images are bundled.
 * Compile against a local SameBoy libsameboy.a; captures belong in an ignored
 * build directory. Usage: capture CARTRIDGE BOOT_ROM OUTPUT_DIRECTORY FRAMES
 */
#include "Core/gb.h"
#include "Core/display.h"
#include "Core/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static uint64_t clocks;
static bool write_register(GB_gameboy_t *gb, uint16_t address, uint8_t value) {
    (void)gb;
    if (address == 0xff14 && (value & 0x80))
        fprintf(stderr, "pulse trigger near frame %.3f, value %02x\n", clocks / 140448.0, value);
    return true;
}

static uint32_t rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb;
    return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}
static int ppm(const char *path, const uint32_t *pixels) {
    FILE *out = fopen(path, "wbx");
    if (!out) return 0;
    fprintf(out, "P6\n160 144\n255\n");
    for (unsigned i = 0; i < 160 * 144; ++i) {
        unsigned char bytes[] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
        if (fwrite(bytes, 1, 3, out) != 3) { fclose(out); return 0; }
    }
    return fclose(out) == 0;
}
int main(int argc, char **argv) {
    if (argc != 5) return 2;
    unsigned frames = (unsigned)strtoul(argv[4], NULL, 10);
    if (!frames || frames > 1000) return 2;
    GB_gameboy_t *gb = GB_init(GB_alloc(), GB_MODEL_CGB_E);
    if (!gb) return 3;
    uint32_t pixels[160 * 144] = {0};
    GB_set_pixels_output(gb, pixels);
    GB_set_rgb_encode_callback(gb, rgb);
    GB_set_color_correction_mode(gb, GB_COLOR_CORRECTION_DISABLED);
    GB_set_write_memory_callback(gb, write_register);
    if (GB_load_rom(gb, argv[1]) || GB_load_boot_rom(gb, argv[2])) return 4;
    puts("frame,ink,left,top,right,bottom,background,center");
    for (unsigned frame = 0; frame < frames; ++frame) {
        const uint64_t end = clocks + 140448;
        while (clocks < end) clocks += GB_run(gb);
        unsigned ink = 0, left = 160, top = 144, right = 0, bottom = 0;
        const uint32_t background = pixels[0];
        for (unsigned y = 0; y < 144; ++y) for (unsigned x = 0; x < 160; ++x) {
            if (pixels[y * 160 + x] == background) continue;
            ++ink;
            if (x < left) left = x;
            if (x > right) right = x;
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
        printf("%u,%u,%u,%u,%u,%u,%06x,%06x\n", frame, ink, left, top, right, bottom,
               background, pixels[64 * 160 + 80]);
        if (frame % 10 == 0) {
            char path[4096];
            if (snprintf(path, sizeof path, "%s/frame-%03u.ppm", argv[3], frame) >= (int)sizeof path ||
                !ppm(path, pixels)) return 5;
        }
    }
    GB_free(gb);
    free(gb);
    return 0;
}
