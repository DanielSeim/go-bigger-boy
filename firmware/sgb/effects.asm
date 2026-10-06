; SPDX-License-Identifier: GPL-3.0-or-later
; Authored effect modulation at fixed $0640, once per physical timer-0 tick.
; A04 rises/A05 falls by one pitch high-byte unit; clamp to 01..3F.
; B04 alternates base pitch +/-2; B05 alternates gain 60/20, two ticks per phase.
; $21/$22 base pitches, $26 A cursor, $27 B phase. Main driver resets on retrigger.
.org $0640
    mov a, $14
    beq effect_b
    mov a, $15
    cmp a, #$04
    beq rise
    cmp a, #$05
    bne effect_b
    mov a, $26
    cmp a, #$01
    beq write_a
    clrc
    adc a, #$ff
    bra write_a
rise:
    mov a, $26
    cmp a, #$3f
    beq write_a
    clrc
    adc a, #$01
write_a:
    mov $26, a
    mov $f2, #$63
    mov $f3, a
effect_b:
    mov a, $18
    cmp a, #$04
    beq vibrato
    cmp a, #$05
    bne done
    mov a, $27
    clrc
    adc a, #$01
    and a, #$03
    mov $27, a
    and a, #$02
    beq gain_high
    mov a, #$20
    bra write_gain
gain_high:
    mov a, #$60
write_gain:
    mov $f2, #$57
    mov $f3, a
    ret
vibrato:
    mov a, $27
    clrc
    adc a, #$01
    and a, #$03
    mov $27, a
    and a, #$02
    beq pitch_high
    mov a, $22
    clrc
    adc a, #$fe
    bra write_b
pitch_high:
    mov a, $22
    clrc
    adc a, #$02
write_b:
    mov $f2, #$53
    mov $f3, a
done:
    ret
