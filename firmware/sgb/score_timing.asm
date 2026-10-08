; SPDX-License-Identifier: GPL-3.0-or-later
; $B1/B2 inherited duration, $B3/B4 inherited articulation, channels 2/3.
; Zero means no qualified initial setup. Calls preserve the current track state.
.org $1610
timing_init:
    mov $b1, #$00
    mov $b2, #$00
    mov $b3, #$00
    mov $b4, #$00
    ret
timing_track:
    mov a, $49
    cmp a, #$02
    bne timing_track3
    mov a, $b1
    mov $68, a
    mov a, $b3
    bra timing_art
timing_track3:
    mov a, $b2
    mov $68, a
    mov a, $b4
timing_art:
    mov $69, a
    ret
; Walk bounded expanded caches in score-tick order until the first active end.
; Only executed events update carry; inactive tracks and clipped tails retain it.
; This uses runtime scratch before the real complete-bank silent rehearsal.
.org $1700
timing_pattern:
    mov a, $46
    xcn a
    mov $61, a
    clrc
    adc a, $61
    mov $61, a
    clrc
    adc a, $61
    mov $61, a
    clrc
    adc a, #$20
    mov $62, a
    mov a, $a2
    and a, #$04
    beq timing_start3
    call timing_load2
timing_start3:
    mov a, $a2
    and a, #$08
    beq timing_tick
    call timing_load3
timing_tick:
    mov a, $a2
    and a, #$04
    beq timing_check3
    dec $32
    mov a, $32
    bne timing_check3
    mov x, $61
    mov a, $4200+x
    beq timing_done
timing_check3:
    mov a, $a2
    and a, #$08
    beq timing_next2
    dec $33
    mov a, $33
    bne timing_next2
    mov x, $62
    mov a, $4200+x
    beq timing_done
timing_next2:
    mov a, $a2
    and a, #$04
    beq timing_next3
    mov a, $32
    bne timing_next3
    call timing_load2
timing_next3:
    mov a, $a2
    and a, #$08
    beq timing_continue
    mov a, $33
    bne timing_continue
    call timing_load3
timing_continue:
    bra timing_tick
timing_done:
    ret
timing_load2:
    mov x, $61
    mov a, $4200+x
    mov $32, a
    mov $b1, a
    mov a, $4400+x
    mov $b3, a
    inc $61
    inc $61
    ret
timing_load3:
    mov x, $62
    mov a, $4200+x
    mov $33, a
    mov $b2, a
    mov a, $4400+x
    mov $b4, a
    inc $62
    inc $62
    ret
