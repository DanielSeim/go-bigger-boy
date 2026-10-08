; SPDX-License-Identifier: GPL-3.0-or-later
; At first end apply only the ending channel's cached zero-time mix fields.
; Channel 2 wins simultaneous endings. Inactive/clipped peers do not execute.
; Reuse rest resolution to update inherited state without VOL, KON or logging.
tail_end:
    mov a, $32
    bne tail_voice3
    mov $23, #$02
    mov a, $61
    bra tail_cursor
tail_voice3:
    mov $23, #$03
    mov a, $62
tail_cursor:
    mov $63, a
    inc $63
    inc $63
    call mix_event
    mov $26, #$c9
    call inherit_event
    ret
