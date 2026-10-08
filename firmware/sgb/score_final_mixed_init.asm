; SPDX-License-Identifier: GPL-3.0-or-later
; Uniform controls or the measured held-127 to returning-63 transition.
final_mixed_art_init:
    mov a, $4480
    cmp a, $bf
    beq final_mixed_art_valid
    cmp a, #$3f
    bne final_return_art_bad
final_mixed_art_valid:
    mov $c1, a
    mov a, #$01
    ret
