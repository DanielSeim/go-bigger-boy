; SPDX-License-Identifier: GPL-3.0-or-later
; Isolated full-duration owned audio for the two-pattern phrase subset.
; Voice 2 is left-only, voice 3 right-only, both source 0 with direct gain 127.
; These diagnostic routing/envelopes do not implement the validated raw controls.
; Notes $98/$99/$A4 only, rests $C9, durations >=2; no calibrated gate profile.
; $50 pitch-low scratch, $51 combined KON mask, $52 release-wait counter.
duet_validate:
    cmp a, #$98
    beq duet_valid
    cmp a, #$99
    beq duet_valid
    cmp a, #$a4
    beq duet_valid
    cmp a, #$c9
    beq duet_valid
    jmp pair_reject
duet_valid:
    ret
duet_setup:
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    mov $f2, #$0c
    mov $f3, #$7f
    mov $f2, #$1c
    mov $f3, #$7f
    mov $f2, #$2c
    mov $f3, #$00
    mov $f2, #$3c
    mov $f3, #$00
    mov $f2, #$2d
    mov $f3, #$00
    mov $f2, #$3d
    mov $f3, #$00
    mov $f2, #$4d
    mov $f3, #$00
    mov $f2, #$5d
    mov $f3, #$10
    mov $f2, #$20
    mov $f3, #$50
    mov $f2, #$21
    mov $f3, #$00
    mov $f2, #$24
    mov $f3, #$00
    mov $f2, #$25
    mov $f3, #$00
    mov $f2, #$26
    mov $f3, #$00
    mov $f2, #$27
    mov $f3, #$7f
    mov $f2, #$30
    mov $f3, #$00
    mov $f2, #$31
    mov $f3, #$50
    mov $f2, #$34
    mov $f3, #$00
    mov $f2, #$35
    mov $f3, #$00
    mov $f2, #$36
    mov $f3, #$00
    mov $f2, #$37
    mov $f3, #$7f
    mov $f2, #$6c
    mov $f3, #$20
    ret
duet_stop:
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$5c
    mov $f3, #$0c
    ret
duet_wait:
    ; Hold KOF across more than two DSP frames before a combined re-key.
    mov $52, #$10
duet_wait_loop:
    dec $52
    mov a, $52
    bne duet_wait_loop
    ret
duet_voice:
    mov a, $26
    cmp a, #$c9
    beq duet_voice_done
    cmp a, #$98
    beq duet_note24
    cmp a, #$99
    beq duet_note25
    mov $50, #$5c
    mov a, #$08
    mov x, a
    bra duet_pitch
duet_note24:
    mov $50, #$2c
    mov a, #$04
    mov x, a
    bra duet_pitch
duet_note25:
    mov $50, #$6c
    mov a, #$04
    mov x, a
duet_pitch:
    mov a, $23
    cmp a, #$02
    bne duet_voice3
    mov $f2, #$22
    mov a, $50
    mov $f3, a
    mov $f2, #$23
    mov a, x
    mov $f3, a
    mov a, $51
    or a, #$04
    mov $51, a
    ret
duet_voice3:
    mov $f2, #$32
    mov a, $50
    mov $f3, a
    mov $f2, #$33
    mov a, x
    mov $f3, a
    mov a, $51
    or a, #$08
    mov $51, a
duet_voice_done:
    ret
duet_on:
    mov a, $51
    beq duet_on_done
    mov $f2, #$5c
    mov $f3, #$00
    mov $f2, #$4c
    mov a, $51
    mov $f3, a
duet_on_done:
    ret
.org $1000
; Independently authored looping square-wave BRR. No original samples.
.byte $10, $10, $10, $10
.org $1010
.byte $b3, $77, $77, $77, $77, $99, $99, $99, $99
