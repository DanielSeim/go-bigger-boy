; SPDX-License-Identifier: GPL-3.0-or-later
final_complete:
    mov a, $ba
    bne final_ready
    jmp poly_complete
final_ready:
    ; Match the observed stop order while preserving an accumulated note.
    ; Gate processing ends with the score; the final note has no timer KOF.
    call gates_init
    mov $70, #$04
    mov $71, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    mov $f3, #$00
    mov $f2, #$4c
    mov a, $51
    mov $f3, a
    beq final_clear
    ; Await actual DSP attack visibility before clearing KON. This preserves
    ; the write across the DSP's sample latch without a calibrated delay.
    mov $f2, #$28
final_attack:
    mov a, $f3
    beq final_attack
final_clear:
    mov $f2, #$4c
    mov $f3, #$00
    ret
