; SPDX-License-Identifier: GPL-3.0-or-later
; $BA final-ready mode. This path is limited to the measured single pattern.
final_guard:
    mov a, $12
    cmp a, #$60
    bne final_bad
    mov a, $46
    cmp a, #$01
    bne final_bad
    mov a, $61
    cmp a, #$04
    bne final_bad
    mov a, $62
    cmp a, #$24
    bne final_bad
    mov a, $4202
    cmp a, #$10
    bne final_bad
    mov a, $4222
    cmp a, #$10
    bne final_bad
    mov $ba, #$01
    mov $b5, #$01
    ret
final_bad:
    jmp pair_reject
