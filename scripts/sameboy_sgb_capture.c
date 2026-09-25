/*
 * Optional black-box visual reference producer for SameBoy v1.0.3.
 * Uses only its public Core API; no ROM or SameBoy code is bundled.
 * Usage: sameboy_sgb_capture ROM BOOT_ROM FRAMES OUTPUT.ppm [sgb|sgb2]
 */
#include "Core/gb.h"
#include "Core/display.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t encode_rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s ROM BOOT_ROM FRAMES OUTPUT.ppm [sgb|sgb2]\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    unsigned long frames = strtoul(argv[3], &end, 10);
    if (!frames || !end || *end || frames > 100000) return 2;
    GB_model_t model = GB_MODEL_SGB;
    if (argc == 6) {
        if (!strcmp(argv[5], "sgb2")) model = GB_MODEL_SGB2;
        else if (strcmp(argv[5], "sgb")) return 2;
    }
    GB_gameboy_t *gb = GB_init(GB_alloc(), model);
    if (!gb) return 3;
    if (GB_load_boot_rom(gb, argv[2])) {
        fprintf(stderr, "could not load reference boot ROM\n");
        return 4;
    }
    GB_set_border_mode(gb, GB_BORDER_ALWAYS);
    GB_set_color_correction_mode(gb, GB_COLOR_CORRECTION_DISABLED);
    GB_set_rgb_encode_callback(gb, encode_rgb);
    const unsigned width = GB_get_screen_width(gb);
    const unsigned height = GB_get_screen_height(gb);
    if (width != 256 || height != 224) {
        fprintf(stderr, "unexpected SameBoy SGB frame size %ux%u\n", width, height);
        return 4;
    }
    uint32_t *pixels = calloc((size_t)width * height, sizeof(*pixels));
    if (!pixels) return 3;
    GB_set_pixels_output(gb, pixels);
    if (GB_load_rom(gb, argv[1])) {
        fprintf(stderr, "could not load ROM\n");
        return 4;
    }
    for (unsigned long frame = 0; frame < frames; ++frame) GB_run_frame(gb);
    FILE *out = fopen(argv[4], "wb");
    if (!out) return 4;
    fprintf(out, "P6\n%u %u\n255\n", width, height);
    for (size_t index = 0; index < (size_t)width * height; ++index) {
        uint8_t rgb[] = {
            (uint8_t)(pixels[index] >> 16),
            (uint8_t)(pixels[index] >> 8),
            (uint8_t)pixels[index],
        };
        if (fwrite(rgb, 1, sizeof(rgb), out) != sizeof(rgb)) return 4;
    }
    const int result = fclose(out);
    free(pixels);
    GB_free(gb);
    GB_dealloc(gb);
    return result ? 4 : 0;
}
