; SPDX-License-Identifier: GPL-3.0-or-later
; D9: the initial prepared note already occupies the first logical score tick.
; Consume that tick once after real onset; silent rehearsal and timer gates
; retain their preceding paths. Qualified initial durations exceed one tick.
initial_score_start:
    call multi_begin
    inc $10
    jmp pair_tick
