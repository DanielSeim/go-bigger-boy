; SPDX-License-Identifier: GPL-3.0-or-later
; Select only the measured two-note clipping geometry, optionally followed
; by one solo pattern on the ending channel. Other scores retain completion.
final_peer_guard:
    mov $bb, #$00
    mov a, $12
    cmp a, #$60
    bne final_peer_return
    mov a, $46
    cmp a, #$01
    beq final_peer_first
    cmp a, #$02
    bne final_peer_return
final_peer_first:
    mov a, $4b00
    cmp a, #$0c
    bne final_peer_return
    mov a, #$00
    mov x, a
    call final_peer_pair
    bne final_peer_return
    mov a, #$20
    mov x, a
    call final_peer_pair
    bne final_peer_return
    mov a, $4202
    cmp a, #$18
    beq final_peer_end3
    cmp a, #$10
    bne final_peer_return
    mov a, $4222
    cmp a, #$18
    bne final_peer_return
    mov $bc, #$04
    mov a, #$40
    mov x, a
    bra final_peer_next
final_peer_end3:
    mov a, $4222
    cmp a, #$10
    bne final_peer_return
    mov $bc, #$08
    mov a, #$60
    mov x, a
final_peer_next:
    mov a, $46
    cmp a, #$01
    beq final_peer_accept
    mov a, $4b01
    cmp a, $bc
    bne final_peer_return
    mov a, $4200+x
    cmp a, #$10
    bne final_peer_return
    mov a, $4201+x
    cmp a, #$9a
    bne final_peer_return
    mov a, $4202+x
    cmp a, #$10
    bne final_peer_return
    mov a, $4203+x
    cmp a, #$9b
    bne final_peer_return
    mov a, $4204+x
    bne final_peer_return
final_peer_accept:
    mov $bb, #$01
final_peer_return:
    ret
final_peer_pair:
    mov a, $4200+x
    cmp a, #$10
    bne final_peer_pair_return
    mov a, $4201+x
    cmp a, #$98
    bne final_peer_pair_return
    mov a, $4203+x
    cmp a, #$99
    bne final_peer_pair_return
    mov a, $4204+x
final_peer_pair_return:
    ret
