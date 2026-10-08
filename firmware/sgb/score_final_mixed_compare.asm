; SPDX-License-Identifier: GPL-3.0-or-later
final_mixed_art_compare:
    mov $c2, a
    mov a, $c0
    cmp a, #$06
    bcc final_mixed_art_initial
    mov a, $c2
    cmp a, $c1
    ret
final_mixed_art_initial:
    mov a, $c2
    cmp a, $bf
    ret
