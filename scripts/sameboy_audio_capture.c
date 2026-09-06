/*
 * Minimal SameBoy v1.0.3 fixture producer used by audio_reference.py.
 *
 * Build this file against the pinned SameBoy checkout and its lib target;
 * it intentionally uses only SameBoy's public API. The register sequences
 * mirror tests/apu_waveform_contract_tests.cpp.
 */
#include "Core/gb.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void discard_sample(GB_gameboy_t *gb, GB_sample_t *sample) {
    (void)gb;
    (void)sample;
}

static int model_for(const char *name, GB_model_t *model) {
    /* SameBoy v1.0.3 intentionally does not expose a DMG-0 model. Keep the
     * distinction explicit instead of silently producing a DMG-B capture. */
    if (!strcmp(name, "dmg0")) return 0;
    if (!strcmp(name, "dmg") || !strcmp(name, "dmg-b")) {
        *model = GB_MODEL_DMG_B;
        return 1;
    }
    if (!strcmp(name, "mgb")) {
        *model = GB_MODEL_MGB;
        return 1;
    }
    if (!strcmp(name, "cgb0")) {
        *model = GB_MODEL_CGB_0;
        return 1;
    }
    if (!strcmp(name, "cgbc") || !strcmp(name, "cgb-c")) {
        *model = GB_MODEL_CGB_C;
        return 1;
    }
    if (!strcmp(name, "cgbe") || !strcmp(name, "cgb-e") || !strcmp(name, "cgb")) {
        *model = GB_MODEL_CGB_E;
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    GB_model_t model = GB_MODEL_DMG_B;
    if (!model_for(argv[1], &model)) {
        fprintf(stderr, "unsupported SameBoy model '%s' (DMG-0 is unavailable in v1.0.3)\n", argv[1]);
        return 2;
    }
    uint8_t rom[0x8000] = {0};
    memcpy(rom + 0x134, "GBB APU REFERENCE", 17);
    GB_gameboy_t *gb = GB_init(GB_alloc(), model);
    if (!gb) return 3;
    GB_load_rom_from_buffer(gb, rom, sizeof rom);
    GB_set_sample_rate(gb, 96000);
    GB_set_highpass_filter_mode(gb, GB_HIGHPASS_ACCURATE);
    GB_apu_set_sample_callback(gb, discard_sample);
    if (GB_start_audio_recording(gb, argv[3], GB_AUDIO_FORMAT_WAV)) return 4;
    GB_write_memory(gb, 0xFF26, 0x80);
    GB_write_memory(gb, 0xFF24, 0x77);
    if (!strcmp(argv[2], "pulse")) {
        GB_write_memory(gb, 0xFF25, 0x11);
        GB_write_memory(gb, 0xFF11, 0x80);
        GB_write_memory(gb, 0xFF12, 0xF0);
        GB_write_memory(gb, 0xFF13, 0xE8);
        GB_write_memory(gb, 0xFF14, 0x87);
    } else if (!strcmp(argv[2], "wave")) {
        GB_write_memory(gb, 0xFF25, 0x44);
        for (unsigned i = 0; i < 16; ++i)
            GB_write_memory(gb, 0xFF30 + i, (uint8_t)(0xF0 - i * 7));
        GB_write_memory(gb, 0xFF1A, 0x80);
        GB_write_memory(gb, 0xFF1C, 0x20);
        GB_write_memory(gb, 0xFF1D, 0x00);
        GB_write_memory(gb, 0xFF1E, 0x87);
    } else if (!strcmp(argv[2], "noise")) {
        GB_write_memory(gb, 0xFF25, 0x88);
        GB_write_memory(gb, 0xFF21, 0xF0);
        GB_write_memory(gb, 0xFF22, 0x08);
        GB_write_memory(gb, 0xFF23, 0x80);
    } else {
        return 2;
    }
    unsigned elapsed = 0;
    /* Capture extra samples so the conversion workflow can trim a
     * deterministic startup prefix while retaining 93 complete 48 kHz
     * frames after the explicit 96 -> 48 kHz downsample. */
    while (elapsed < 20000) elapsed += GB_run(gb);
    const int result = GB_stop_audio_recording(gb);
    GB_free(gb);
    GB_dealloc(gb);
    return result ? 5 : 0;
}
