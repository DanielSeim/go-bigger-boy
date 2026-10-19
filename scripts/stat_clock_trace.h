/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded host-side observations shared by the optional clock adapters.
 * Request intervals are (last observation clear, first observation pending].
 * No callback advances emulated time or writes emulated memory.
 */
#ifndef GBB_STAT_CLOCK_TRACE_H
#define GBB_STAT_CLOCK_TRACE_H
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#define STAT_CLOCK_MAX_STEPS 512
#define STAT_CLOCK_LIMIT UINT64_C(20000000)
typedef struct {
    uint64_t begin, end;
    uint16_t pc_before, pc_after, bc_before, bc_after;
    unsigned if_before, if_after;
} StatClockStep;
typedef struct {
    int active, lcd_seen, request_seen, vector_seen, dispatch_seen, overflow;
    uint64_t origin, lcd_clock, last_clear, request_lower, request_upper, vector_clock;
    int sample_seen, sample_after_fetch;
    uint64_t sample_clock;
    unsigned sample_pc, sample_pending;
    StatClockStep dispatch, steps[STAT_CLOCK_MAX_STEPS];
    unsigned count;
} StatClockTrace;
static inline void stat_clock_observe(StatClockTrace *s, uint64_t clock, unsigned flags) {
    if (!s->active || s->request_seen) return;
    if (!(flags & 2)) s->last_clear = clock;
    else {
        s->request_seen = 1;
        s->request_lower = s->last_clear;
        s->request_upper = clock;
    }
}
static inline void stat_clock_lcd(StatClockTrace *s, uint64_t clock) {
    if (s->active && !s->lcd_seen) { s->lcd_seen = 1; s->lcd_clock = clock; }
}
static inline void stat_clock_vector(StatClockTrace *s, uint64_t clock, uint16_t pc) {
    if (s->active && !s->vector_seen && pc == 0x48) {
        s->vector_seen = 1; s->vector_clock = clock;
    }
}
static inline void stat_clock_sample(StatClockTrace *s, uint64_t clock, uint16_t pc,
                                     unsigned pending, int after_fetch) {
    if (!s->active || s->sample_seen) return;
    s->sample_seen=1; s->sample_clock=clock; s->sample_pc=pc;
    s->sample_pending=pending; s->sample_after_fetch=after_fetch;
}
static inline void stat_clock_step(StatClockTrace *s, StatClockStep step) {
    if (!s->active) return;
    if (s->count == STAT_CLOCK_MAX_STEPS) { s->overflow = 1; return; }
    s->steps[s->count++] = step;
    if (!s->dispatch_seen && step.pc_after == 0x48 && step.pc_before != 0x48) {
        s->dispatch_seen = 1; s->dispatch = step;
    }
}
static inline uint64_t stat_clock_hash(const uint8_t *bytes, size_t count) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i=0;i<count;++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static inline void stat_clock_print_step(const StatClockStep *p) {
    printf("{\"begin\":%" PRIu64 ",\"end\":%" PRIu64 ",\"pc_before\":%u,\"pc_after\":%u,"
           "\"bc_before\":%u,\"bc_after\":%u,\"if_before\":%u,\"if_after\":%u}",
           p->begin,p->end,p->pc_before,p->pc_after,p->bc_before,p->bc_after,p->if_before,p->if_after);
}
static inline void stat_clock_print(const StatClockTrace *s, const char *core, const char *phase,
                                    int enabled, int complete, uint64_t clock,
                                    const uint16_t regs[6], const uint8_t wram[64], size_t state_size, uint64_t state_hash, unsigned excluded_host_bytes) {
    printf("{\"schema\":\"gbb-stat-clock-trace-v1\",\"qualification\":false,\"core\":\"%s\","
           "\"clock_unit\":\"8mhz\",\"anchor\":\"before_step_pc_1000\","
           "\"model\":\"cgb-e\",\"opcode_callback_phase\":\"%s\","
           "\"observers_enabled\":%s,\"complete\":%s,\"overflow\":%s,\"window_seen\":%s,"
           "\"window_clock\":%" PRIu64 ",\"lcd_seen\":%s,\"lcd_accept_clock\":%" PRIu64 ","
           "\"request_seen\":%s,\"request_lower_exclusive\":%" PRIu64 ",\"request_upper_inclusive\":%" PRIu64 ","
           "\"vector_seen\":%s,\"vector_opcode_clock\":%" PRIu64 ",\"dispatch_seen\":%s,\"dispatch\":",
           core,phase,enabled?"true":"false",complete?"true":"false",s->overflow?"true":"false",
           s->active?"true":"false",s->origin,s->lcd_seen?"true":"false",s->lcd_clock,
           s->request_seen?"true":"false",s->request_lower,s->request_upper,
           s->vector_seen?"true":"false",s->vector_clock,s->dispatch_seen?"true":"false");
    stat_clock_print_step(&s->dispatch);
    printf(",\"interrupt_sample_seen\":%s,\"interrupt_sample_clock\":%" PRIu64
           ",\"interrupt_sample_pc\":%u,\"interrupt_sample_pending\":%u,\"interrupt_sample_after_fetch\":%s",
           s->sample_seen?"true":"false",s->sample_clock,s->sample_pc,s->sample_pending,
           s->sample_after_fetch?"true":"false");
    printf(",\"final_clock\":%" PRIu64 ",\"state_bytes\":%zu,\"state_fnv1a64\":%" PRIu64 ",\"state_excluded_host_bytes\":%u,\"registers\":[",clock,state_size,state_hash,excluded_host_bytes);
    for (unsigned i=0;i<6;++i) printf("%s%u",i?",":"",regs[i]);
    printf("],\"wram\":[");
    for (unsigned i=0;i<64;++i) printf("%s%u",i?",":"",wram[i]);
    printf("],\"steps\":[");
    for (unsigned i=0;i<s->count;++i) { if (i) putchar(','); stat_clock_print_step(&s->steps[i]); }
    puts("]}");
}
#endif
