; SPDX-License-Identifier: GPL-3.0-or-later
final_return_geometry:
    mov a, $4200
    cmp a, #$10
    bne final_return_geometry_bad
    mov a, $4220
    cmp a, #$10
    bne final_return_geometry_bad
    mov a, $4202
    cmp a, #$18
    bne final_return_geometry_bad
    mov a, $4222
    cmp a, #$10
    bne final_return_geometry_bad
    mov a, $4204
    bne final_return_geometry_bad
    mov a, $4224
    bne final_return_geometry_bad
    mov a, $4240
    bne final_return_geometry_bad
    mov a, $4260
    cmp a, #$10
    bne final_return_geometry_bad
    mov a, $4262
    cmp a, #$10
    bne final_return_geometry_bad
    mov a, $4264
    bne final_return_geometry_bad
    mov a, #$01
    ret
final_return_geometry_bad:
    mov a, #$00
    ret
