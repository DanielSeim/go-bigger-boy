; SPDX-License-Identifier: GPL-3.0-or-later
final_return_boundary:
    mov a, $4282
    cmp a, #$08
    beq final_return_boundary_note
    cmp a, #$10
    bne final_return_boundary_bad
final_return_boundary_note:
    mov a, $4283
    cmp a, #$c9
    beq final_return_boundary_end
    cmp a, #$a0
    bne final_return_boundary_bad
final_return_boundary_end:
    mov a, $4284
    bne final_return_boundary_bad
    mov a, $42a2
    bne final_return_boundary_bad
    mov a, #$01
    ret
final_return_boundary_bad:
    mov a, #$00
    ret
