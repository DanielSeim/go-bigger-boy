/*
 * Deterministic SameBoy revision-boundary probe used during APU review.
 *
 * Build against the pinned SameBoy checkout (see
 * tests/fixtures/audio-external/sameboy-reference-pin.json), then run:
 *
 *   sameboy_revision_probe cgb-c alignment /tmp/cgb-c-alignment.txt
 *
 * The output records CPU-run boundaries and the public PCM12/PCM34 reads. It
 * is intentionally a diagnostic fixture producer, not a release dependency.
 */
#include "Core/gb.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int model_for(const char *name, GB_model_t *model) {
    if (!strcmp(name, "cgb0")) *model = GB_MODEL_CGB_0;
    else if (!strcmp(name, "cgb-c")) *model = GB_MODEL_CGB_C;
    else if (!strcmp(name, "cgb-e")) *model = GB_MODEL_CGB_E;
    else return 0;
    return 1;
}

static void write_register(GB_gameboy_t *gb, const uint16_t address,
                           const uint8_t value) {
    GB_write_memory(gb, address, value);
}

static void setup_apu(GB_gameboy_t *gb) {
    write_register(gb, 0xFF26, 0x80);
    write_register(gb, 0xFF24, 0x77);
}

static void setup_pulse2(GB_gameboy_t *gb) {
    setup_apu(gb);
    write_register(gb, 0xFF16, 0x80);
    write_register(gb, 0xFF17, 0x80);
    write_register(gb, 0xFF18, 0xF8);
    write_register(gb, 0xFF19, 0x87);
}

static void setup_noise(GB_gameboy_t *gb) {
    setup_apu(gb);
    write_register(gb, 0xFF25, 0x88);
    write_register(gb, 0xFF21, 0xF0);
    write_register(gb, 0xFF22, 0x08);
    write_register(gb, 0xFF23, 0x80);
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    GB_model_t model;
    if (!model_for(argv[1], &model)) return 2;

    uint8_t rom[0x8000] = {0};
    GB_gameboy_t *gb = GB_init(GB_alloc(), model);
    if (!gb) return 3;
    GB_load_rom_from_buffer(gb, rom, sizeof rom);

    if (!strcmp(argv[2], "alignment")) setup_pulse2(gb);
    else if (!strcmp(argv[2], "pcm") || !strcmp(argv[2], "lfsr"))
        setup_noise(gb);
    else {
        GB_free(gb);
        GB_dealloc(gb);
        return 2;
    }

    FILE *output = fopen(argv[3], "w");
    if (!output) {
        GB_free(gb);
        GB_dealloc(gb);
        return 4;
    }
    fprintf(output, "model=%s fixture=%s\n", argv[1], argv[2]);
    for (unsigned step = 0; step < 128; ++step) {
        /* Step 5 lands on the first channel-4 reload window in the pinned
         * SameBoy run loop; this is the deliberately awkward NR43 boundary
         * used by the revision review. */
        if (!strcmp(argv[2], "lfsr") && step == 5)
            write_register(gb, 0xFF22, 0x18);
        if (!strcmp(argv[2], "pcm") && step == 32)
            write_register(gb, 0xFF22, 0x18);
        const unsigned cycles = GB_run(gb);
        fprintf(output, "step=%u cycles=%u pcm12=%02x pcm34=%02x\n", step,
                cycles, GB_read_memory(gb, 0xFF76),
                GB_read_memory(gb, 0xFF77));
    }
    fclose(output);
    GB_free(gb);
    GB_dealloc(gb);
    return 0;
}
