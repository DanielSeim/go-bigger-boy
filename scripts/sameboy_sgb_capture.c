/*
 * Optional black-box visual reference producer for SameBoy v1.0.3.
 * Uses only its public Core API; no ROM or SameBoy code is bundled.
 * Usage: sameboy_sgb_capture ROM BOOT_ROM FRAMES OUTPUT.ppm [sgb|sgb2]
 *        [--input-script PATH --input-offset FRAMES]
 *        [--input-offset-at SCRIPT_FRAME FRAMES]...
 *        [--random-seed UNSIGNED_DECIMAL]
 *    or: sameboy_sgb_capture ROM BOOT_ROM --series FIRST LAST PREFIX [sgb|sgb2]
 *        [--input-script PATH --input-offset FRAMES]
 *        [--input-offset-at SCRIPT_FRAME FRAMES]...
 *        [--random-seed UNSIGNED_DECIMAL]
 */
#include "Core/gb.h"
#include "Core/display.h"
#include "sgb_input_script.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t encode_rgb(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    (void)gb;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static int write_ppm(const char *path, const uint32_t *pixels,
                     unsigned width, unsigned height) {
    FILE *out = fopen(path, "wb");
    if (!out) return 4;
    if (fprintf(out, "P6\n%u %u\n255\n", width, height) < 0) {
        fclose(out);
        return 4;
    }
    for (size_t index = 0; index < (size_t)width * height; ++index) {
        uint8_t rgb[] = {
            (uint8_t)(pixels[index] >> 16),
            (uint8_t)(pixels[index] >> 8),
            (uint8_t)pixels[index],
        };
        if (fwrite(rgb, 1, sizeof(rgb), out) != sizeof(rgb)) {
            fclose(out);
            return 4;
        }
    }
    return fclose(out) ? 4 : 0;
}

static void set_held_buttons(GB_gameboy_t *gb, uint8_t previous,
                             uint8_t next) {
    for (unsigned bit = 0; bit < 8; ++bit) {
        uint8_t flag = (uint8_t)(1u << bit);
        if ((previous & flag) != (next & flag)) {
            GB_set_key_state(gb, (GB_key_t)bit, (next & flag) != 0);
        }
    }
}

int main(int argc, char **argv) {
    const int series = argc >= 4 && !strcmp(argv[3], "--series");
    const int positional = series ? 7 : 5;
    if (argc < positional) {
        fprintf(stderr, "usage: %s ROM BOOT_ROM FRAMES OUTPUT.ppm [sgb|sgb2]\n"
                "   or: %s ROM BOOT_ROM --series FIRST LAST PREFIX [sgb|sgb2]\n"
                "   optional: --input-script PATH --input-offset FRAMES\n"
                "             [--input-offset-at SCRIPT_FRAME FRAMES]...\n"
                "             [--random-seed UNSIGNED_DECIMAL]\n",
                argv[0], argv[0]);
        return 2;
    }
    char *end = NULL;
    unsigned long first = strtoul(argv[series ? 4 : 3], &end, 10);
    if (!first || !end || *end || first > 100000) return 2;
    unsigned long last = first;
    if (series) {
        last = strtoul(argv[5], &end, 10);
        if (!last || !end || *end || last < first ||
            last > 100000 || last - first > 1000) return 2;
    }
    GB_model_t model = GB_MODEL_SGB;
    const char *input_path = NULL;
    unsigned long input_offset = 0;
    int saw_offset = 0;
    uint64_t random_seed = 0;
    int saw_random_seed = 0;
    gbb_sgb_input_offset_map offset_map = {0};
    for (int index = positional; index < argc; ++index) {
        if (!strcmp(argv[index], "sgb") && index == positional) {
            continue;
        }
        if (!strcmp(argv[index], "sgb2") && index == positional) {
            model = GB_MODEL_SGB2;
            continue;
        }
        if (!strcmp(argv[index], "--input-script") && index + 1 < argc &&
            input_path == NULL) {
            input_path = argv[++index];
            continue;
        }
        if (!strcmp(argv[index], "--input-offset") && index + 1 < argc &&
            !saw_offset) {
            const char *value = argv[++index];
            input_offset = strtoul(value, &end, 10);
            if (end == value || *end || input_offset > GBB_SGB_INPUT_MAX_FRAME) return 2;
            saw_offset = 1;
            continue;
        }
        if (!strcmp(argv[index], "--input-offset-at") && index + 2 < argc &&
            offset_map.count < GBB_SGB_INPUT_MAX_OFFSET_CHANGES) {
            const char *frame_text = argv[++index];
            unsigned long script_frame = strtoul(frame_text, &end, 10);
            if (end == frame_text || *end || script_frame == 0 ||
                script_frame > GBB_SGB_INPUT_MAX_FRAME) return 2;
            const char *offset_text = argv[++index];
            unsigned long offset = strtoul(offset_text, &end, 10);
            if (end == offset_text || *end ||
                offset > GBB_SGB_INPUT_MAX_FRAME) return 2;
            offset_map.changes[offset_map.count].frame = (unsigned)script_frame;
            offset_map.changes[offset_map.count].offset = (unsigned)offset;
            ++offset_map.count;
            continue;
        }
        if (!strcmp(argv[index], "--random-seed") && index + 1 < argc &&
            !saw_random_seed) {
            const char *value = argv[++index];
            if (*value < '0' || *value > '9') return 2;
            errno = 0;
            unsigned long long parsed = strtoull(value, &end, 10);
            if (errno == ERANGE || end == value || *end) return 2;
            random_seed = (uint64_t)parsed;
            saw_random_seed = 1;
            continue;
        }
        return 2;
    }
    if ((saw_offset || offset_map.count) && input_path == NULL) return 2;
    offset_map.base_offset = (unsigned)input_offset;
    gbb_sgb_input_script inputs = {0};
    unsigned scheduled_input_frames[GBB_SGB_INPUT_MAX_EVENTS] = {0};
    if (input_path) {
        char error[128] = {0};
        if (!gbb_sgb_input_load(input_path, &inputs, error, sizeof(error))) {
            fprintf(stderr, "input script: %s\n", error);
            return 2;
        }
        if (!gbb_sgb_input_schedule(&inputs, &offset_map,
                                    scheduled_input_frames, error,
                                    sizeof(error))) {
            fprintf(stderr, "input schedule: %s\n", error);
            return 2;
        }
    }
    if (saw_random_seed) GB_random_seed(random_seed);
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
    size_t next_input_event = 0;
    uint8_t held_buttons = 0;
    if (inputs.count && scheduled_input_frames[0] == 0) {
        set_held_buttons(gb, held_buttons, inputs.events[0].mask);
        held_buttons = inputs.events[0].mask;
        ++next_input_event;
    }
    int result = 0;
    for (unsigned long frame = 1; frame <= last; ++frame) {
        GB_run_frame(gb);
        if (frame >= first) {
            if (series) {
                char path[4096];
                if (snprintf(path, sizeof(path), "%s-%06lu.ppm", argv[6], frame) >=
                    (int)sizeof(path)) return 4;
                result = write_ppm(path, pixels, width, height);
            } else {
                result = write_ppm(argv[4], pixels, width, height);
            }
            if (result) break;
        }
        if (next_input_event < inputs.count &&
            frame == scheduled_input_frames[next_input_event]) {
            uint8_t mask = inputs.events[next_input_event++].mask;
            set_held_buttons(gb, held_buttons, mask);
            held_buttons = mask;
        }
    }
    free(pixels);
    GB_free(gb);
    GB_dealloc(gb);
    return result;
}
