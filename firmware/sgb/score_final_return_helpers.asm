; SPDX-License-Identifier: GPL-3.0-or-later
final_return_art:
    mov a, $4400
    mov $bf, a
    mov $c0, #$00
final_return_art_loop:
    mov a, $c0
    mov x, a
    mov a, $0f70+x
    mov x, a
    mov a, $4400+x
    cmp a, $bf
    bne final_return_art_bad
    inc $c0
    mov a, $c0
    cmp a, #$09
    bne final_return_art_loop
    mov a, #$01
    ret
final_return_art_bad:
    mov a, #$00
    ret
