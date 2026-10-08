; SPDX-License-Identifier: GPL-3.0-or-later
; The owned matching-articulation profile reaches here with a live counter
; only at articulation 127. Rests alter that old gate without generating KON.
final_return_rest_gate:
    mov a, $be
    beq final_return_rest_done
    mov a, $9c
    cmp a, #$02
    bne final_return_rest_done
    mov a, $23
    cmp a, #$02
    bne final_return_rest_done
    mov a, $26
    cmp a, #$c9
    bne final_return_rest_done
    mov a, $72
    beq final_return_rest_done
    mov a, $22
    cmp a, #$04
    beq final_return_rest_short
    mov $72, #$0f
    ret
final_return_rest_short:
    mov $72, #$05
final_return_rest_done:
    ret
