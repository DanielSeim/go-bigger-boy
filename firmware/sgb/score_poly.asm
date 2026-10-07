; SPDX-License-Identifier: GPL-3.0-or-later
; Isolated owned full-duration multi-event audio; $70 changed voices, $71 held KOF.
; Reuse owned duet pitch/source/routing and sample. No calibrated gate profiles.
poly_setup:
    call duet_setup
    mov $71, #$0c
    ret
poly_all:
    mov a, $64
    beq poly_return
    mov $70, #$0c
    call poly_release
    ret
poly_prepare:
    mov a, $64
    beq poly_return
    mov $70, #$00
    mov a, $32
    bne poly_check3
    mov $70, #$04
poly_check3:
    mov a, $33
    bne poly_prepared
    mov a, $70
    or a, #$08
    mov $70, a
poly_prepared:
    call poly_release
poly_return:
    ret
poly_release:
    mov $51, #$00
    mov a, $70
    beq poly_return
    ; Remove prior KON data; assert KOF only for changed voices.
    mov $f2, #$4c
    mov $f3, #$00
    mov a, $71
    or a, $70
    mov $71, a
    mov $f2, #$5c
    mov $f3, a
    call duet_wait
    ret
poly_on:
    mov a, $64
    beq poly_return
    mov a, $70
    beq poly_return
    ; Keep rest voices in release, clear KOF only for newly keyed notes.
    mov x, $51
    mov a, $0e00+x
    and a, $71
    mov $71, a
    mov $f2, #$5c
    mov $f3, a
    mov a, $51
    beq poly_return
    mov $f2, #$4c
    mov $f3, a
    ret
poly_complete:
    mov $70, #$0c
    call duet_stop
    ret
.org $0e00
; (~KON) & 12, for masks 0/4/8/12. Other indices are unreachable.
.byte $0c, $00, $00, $00, $08, $00, $00, $00, $04, $00, $00, $00, $00
