; SPDX-License-Identifier: GPL-3.0-or-later
; D7-only selector 2: exact measured octave for instrument 10, notes 24..36.
; $26 is the admitted note opcode $98..A4, $50 low pitch, X high pitch.
; The table changes tuning only; source IDs, samples and envelopes stay owned.
tuning_octave:
    mov a, $26
    clrc
    adc a, #$68 ; $98..A4 -> 0..12 (8-bit wrap)
    mov x, a
    mov a, tuning_low+x
    mov $50, a
    mov a, tuning_high+x
    mov x, a
    ret
tuning_low:
    .byte $39, $18, $15, $30, $68, $bf, $34, $e4, $b3, $9f, $c8, $0e, $90
tuning_high:
    .byte $1f, $21, $23, $25, $27, $29, $2c, $2e, $31, $34, $37, $3b, $3e
