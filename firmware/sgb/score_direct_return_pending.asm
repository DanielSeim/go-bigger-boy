; SPDX-License-Identifier: GPL-3.0-or-later
direct_return_pending_compare:
    mov a, $c3
    beq direct_return_pending_old
    mov a, $4283
    cmp a, #$a1
    ret
direct_return_pending_old:
    mov a, $4283
    cmp a, #$a0
    ret
