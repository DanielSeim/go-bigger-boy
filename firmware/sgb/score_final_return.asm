; SPDX-License-Identifier: GPL-3.0-or-later
; $BE scopes returning-rest timing and final readiness to three patterns.
final_return_profile:
    mov a, $12
    cmp a, #$60
    bne final_return_miss
    mov a, $46
    cmp a, #$03
    bne final_return_miss
    mov a, $4b00
    cmp a, #$0c
    bne final_return_miss
    mov a, $4b01
    cmp a, #$08
    bne final_return_miss
    mov a, $4b02
    cmp a, #$0c
    bne final_return_miss
    mov a, $4280
    cmp a, #$04
    beq final_return_rest_ok
    cmp a, #$08
    bne final_return_miss
final_return_rest_ok:
    mov $bf, a
    mov a, $42a0
    cmp a, $bf
    bne final_return_miss
    mov a, $4281
    cmp a, #$c9
    bne final_return_miss
    mov a, $42a1
    cmp a, #$c9
    bne final_return_miss
    call final_return_geometry
    beq final_return_miss
    call final_return_boundary
    beq final_return_miss
    call final_return_art
    beq final_return_miss
    mov $be, #$01
    ret
final_return_miss:
    mov a, #$00
    ret
