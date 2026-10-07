; SPDX-License-Identifier: GPL-3.0-or-later
; Isolated experimental SPC score clock; not included in the SGB prototype.
; $12 supplied tempo (96/192 only), $10/11 16-bit ticks, $13 fraction,
; $14 status (1 running, E1 rejected), $15 pending timer-0 pulses.
; Never publishes mailbox readiness or touches a score bank.
.org $0800
    mov $f1, #$80
    mov $f2, #$6c
    mov $f3, #$e0
    mov $10, #$00
    mov $11, #$00
    mov $13, #$00
    mov $14, #$00
    mov $15, #$00
    mov a, $12
    cmp a, #$60
    beq clock_start
    cmp a, #$c0
    beq clock_start
    mov $14, #$e1
clock_rejected:
    bra clock_rejected
clock_start:
; Timer 0: 128 prescale * target 16 = 2048 SPC cycles per pulse.
    mov $fa, #$10
    mov $f1, #$81
    mov $14, #$01
clock_poll:
    mov a, $fd
    beq clock_poll
    mov $15, a
clock_pulse:
; Tempo/256 score ticks per timer pulse; preserve the fractional remainder.
    mov a, $13
    clrc
    adc a, $12
    mov $13, a
    bcc clock_next
    inc $10
    mov a, $10
    bne clock_next
    inc $11
clock_next:
    dec $15
    mov a, $15
    bne clock_pulse
    bra clock_poll
