; SPDX-License-Identifier: GPL-3.0-or-later
; Opt-in bounded pacing experiment, not an inferred original timer algorithm.
; 7b: last live tempo, 7c: waiting marker, 7d: remaining ms, 7e: elapsed pulses.
; Cold/reload/explicit music STOP clear history; completion retains it.
vendor_selection_wait:
    mov a, $3d
    beq vendor_selection_done
    mov a, $7b
    beq vendor_selection_done
    .byte $cd, $00
vendor_selection_lookup:
    mov a, vendor_selection_tempos+x
    cmp a, $7b
    beq vendor_selection_found
    .byte $3d
    cmp x, #$05
    bne vendor_selection_lookup
    ret
vendor_selection_found:
    mov a, vendor_selection_milliseconds+x
    mov $7d, a
    mov $7c, #$01
    ; Timer 1: 8 * 125 us. No host clock or frame-count substitution.
    mov $fb, #$08
    mov $f1, #$82
    mov a, $fe
vendor_selection_poll:
    ; STOP, reload and newer selections keep their existing stack ownership.
    call vendor_service
    mov a, $fe
    beq vendor_selection_poll
    mov $7e, a
vendor_selection_pulse:
    dec $7d
    mov a, $7d
    beq vendor_selection_elapsed
    dec $7e
    mov a, $7e
    bne vendor_selection_pulse
    bra vendor_selection_poll
vendor_selection_elapsed:
    mov $f1, #$80
    mov $7c, #$00
vendor_selection_done:
    ret
vendor_selection_tempos:
.byte $20,$2d,$40,$60,$80
vendor_selection_milliseconds:
.byte $20,$1b,$18,$13,$14
