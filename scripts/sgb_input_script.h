#ifndef GBB_SGB_INPUT_SCRIPT_H
#define GBB_SGB_INPUT_SCRIPT_H

/* Shared, ROM-free frame-boundary input format for GBB and the optional
 * SameBoy reference driver. Bit order matches both public joypad enums:
 * right, left, up, down, a, b, select, start. The mask is the complete
 * held-button state after the numbered frame, not a press/release delta. */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GBB_SGB_INPUT_MAX_EVENTS 1024
#define GBB_SGB_INPUT_MAX_FRAME 100000
#define GBB_SGB_INPUT_MAX_OFFSET_CHANGES 16

typedef struct {
    unsigned frame;
    uint8_t mask;
} gbb_sgb_input_event;

typedef struct {
    size_t count;
    gbb_sgb_input_event events[GBB_SGB_INPUT_MAX_EVENTS];
} gbb_sgb_input_script;

typedef struct {
    unsigned frame;
    unsigned offset;
} gbb_sgb_input_offset_change;

typedef struct {
    unsigned base_offset;
    size_t count;
    gbb_sgb_input_offset_change changes[GBB_SGB_INPUT_MAX_OFFSET_CHANGES];
} gbb_sgb_input_offset_map;

static int gbb_sgb_input_error(char *error, size_t capacity,
                               unsigned line, const char *message) {
    if (capacity) snprintf(error, capacity, "line %u: %s", line, message);
    return 0;
}

static int gbb_sgb_input_load(const char *path, gbb_sgb_input_script *script,
                              char *error, size_t capacity) {
    static const char *names[] = {
        "right", "left", "up", "down", "a", "b", "select", "start"};
    FILE *input = fopen(path, "rb");
    if (!input) {
        if (capacity) snprintf(error, capacity, "cannot open input script");
        return 0;
    }
    script->count = 0;
    char line[256];
    unsigned line_number = 0;
    int ok = 1;
    while (fgets(line, sizeof(line), input)) {
        ++line_number;
        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            ok = gbb_sgb_input_error(error, capacity, line_number,
                                      "line is too long");
            break;
        }
        while (length && (line[length - 1] == '\n' ||
                          line[length - 1] == '\r')) line[--length] = '\0';
        if (line_number == 1) {
            if (strcmp(line, "GBB SGB input v1")) {
                ok = gbb_sgb_input_error(error, capacity, line_number,
                                          "expected GBB SGB input v1 header");
                break;
            }
            continue;
        }
        char *cursor = line;
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor || *cursor == '#') continue;
        if (!isdigit((unsigned char)*cursor)) {
            ok = gbb_sgb_input_error(error, capacity, line_number,
                                      "expected frame number");
            break;
        }
        char *end = NULL;
        unsigned long frame = strtoul(cursor, &end, 10);
        if (frame > GBB_SGB_INPUT_MAX_FRAME || end == cursor ||
            !isspace((unsigned char)*end)) {
            ok = gbb_sgb_input_error(error, capacity, line_number,
                                      "invalid frame number");
            break;
        }
        cursor = end;
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) {
            ok = gbb_sgb_input_error(error, capacity, line_number,
                                      "missing held buttons");
            break;
        }
        end = cursor;
        while (*end && !isspace((unsigned char)*end)) ++end;
        if (*end) {
            *end++ = '\0';
            while (isspace((unsigned char)*end)) ++end;
            if (*end && *end != '#') {
                ok = gbb_sgb_input_error(error, capacity, line_number,
                                          "unexpected trailing text");
                break;
            }
        }
        uint8_t mask = 0;
        if (strcmp(cursor, "none")) {
            const size_t token_length = strlen(cursor);
            if (!token_length || cursor[0] == '+' ||
                cursor[token_length - 1] == '+' || strstr(cursor, "++")) {
                ok = gbb_sgb_input_error(error, capacity, line_number,
                                          "invalid button list");
                break;
            }
            for (char *button = strtok(cursor, "+"); button;
                 button = strtok(NULL, "+")) {
                unsigned index = 0;
                while (index < 8 && strcmp(button, names[index])) ++index;
                if (index == 8 || (mask & (1u << index))) {
                    ok = gbb_sgb_input_error(error, capacity, line_number,
                                              "unknown or duplicate button");
                    break;
                }
                mask = (uint8_t)(mask | (1u << index));
            }
            if (!ok) break;
        }
        if (script->count >= GBB_SGB_INPUT_MAX_EVENTS ||
            (script->count && frame <= script->events[script->count - 1].frame)) {
            ok = gbb_sgb_input_error(error, capacity, line_number,
                                      "events must increase and fit the limit");
            break;
        }
        script->events[script->count].frame = (unsigned)frame;
        script->events[script->count].mask = mask;
        ++script->count;
    }
    if (ferror(input)) ok = gbb_sgb_input_error(error, capacity, line_number,
                                                "error reading script");
    if (!line_number) ok = gbb_sgb_input_error(error, capacity, 1,
                                               "missing header");
    fclose(input);
    return ok;
}

/* Reference emulators may count boot and LCD-off frames differently. Apply
 * piecewise offsets to input events, not to captured images, and reject any
 * mapping that reverses or merges events. */
static int gbb_sgb_input_schedule(const gbb_sgb_input_script *script,
                                  const gbb_sgb_input_offset_map *map,
                                  unsigned scheduled[GBB_SGB_INPUT_MAX_EVENTS],
                                  char *error, size_t capacity) {
    if (script->count > GBB_SGB_INPUT_MAX_EVENTS ||
        map->count > GBB_SGB_INPUT_MAX_OFFSET_CHANGES ||
        map->base_offset > GBB_SGB_INPUT_MAX_FRAME) {
        if (capacity) snprintf(error, capacity, "input schedule exceeds limit");
        return 0;
    }
    for (size_t index = 0; index < map->count; ++index) {
        const gbb_sgb_input_offset_change *change = &map->changes[index];
        if (change->frame == 0 || change->frame > GBB_SGB_INPUT_MAX_FRAME ||
            change->offset > GBB_SGB_INPUT_MAX_FRAME ||
            (index && change->frame <= map->changes[index - 1].frame)) {
            if (capacity) snprintf(error, capacity,
                                   "offset changes must have increasing positive frames");
            return 0;
        }
    }
    size_t next_change = 0;
    unsigned offset = map->base_offset;
    for (size_t index = 0; index < script->count; ++index) {
        const unsigned frame = script->events[index].frame;
        while (next_change < map->count &&
               frame >= map->changes[next_change].frame) {
            offset = map->changes[next_change++].offset;
        }
        if (frame > GBB_SGB_INPUT_MAX_FRAME - offset ||
            (index && frame + offset <= scheduled[index - 1])) {
            if (capacity) snprintf(error, capacity,
                                   "mapped input frames must increase and fit the limit");
            return 0;
        }
        scheduled[index] = frame + offset;
    }
    return 1;
}

#endif
