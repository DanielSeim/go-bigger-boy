; SPDX-License-Identifier: GPL-3.0-or-later
; $A2 parser active mask; $A3 runtime active mask, cached at $4B00+pattern.
multi_pattern:
    call poly_all
    mov x, $9c
    mov a, $4b00+x
    mov $a3, a
    mov $70, a
    mov $32, #$01
    mov $33, #$01
    mov a, $30
    mov $61, a
    clrc
    adc a, #$20
    mov $62, a
    mov a, $a3
    and a, #$04
    beq sparse_start3
    call multi_load2
sparse_start3:
    mov a, $a3
    and a, #$08
    beq sparse_start_done
    call multi_load3
sparse_start_done:
    ; Do not consume the initial gate's timer phase in the startup release wait
    ; or note preparation. Subsequent patterns retain the running timer phase.
    mov a, $64
    beq sparse_start_on
    mov a, $9c
    bne sparse_start_on
    mov $fa, #$10
    mov $f1, #$81
sparse_start_on:
    call poly_on
    ret
sparse_tick:
    call list_tick_budget
    mov a, $a3
    cmp a, #$0c
    bne sparse_solo
    jmp sparse_pair_tick
sparse_solo:
    cmp a, #$04
    bne sparse_tick3
    dec $32
    mov a, $32
    bne sparse_tick_done
    mov x, $61
    mov a, $4200+x
    bne sparse_next2
    jmp multi_advance
sparse_next2:
    call poly_prepare
    call multi_load2
    call poly_on
    ret
sparse_tick3:
    dec $33
    mov a, $33
    bne sparse_tick_done
    mov x, $62
    mov a, $4200+x
    bne sparse_next3
    jmp multi_advance
sparse_next3:
    call poly_prepare
    call multi_load3
    call poly_on
sparse_tick_done:
    ret
sparse_song:
    ; Once in the first active track's prefix of the first pattern. A missing
    ; channel 2 must not prevent channel 3 from selecting shared song volume.
    mov a, $46
    bne sparse_song_bad
    mov a, $a2
    bne sparse_song_bad
    jmp sparse_song_guard
sparse_song_bad:
    jmp pair_reject
