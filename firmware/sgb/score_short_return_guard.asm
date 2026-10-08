; SPDX-License-Identifier: GPL-3.0-or-later
short_return_guard:
    mov a, $c4
    beq short_return_guard_done
    mov a, $be
    beq short_return_guard_bad
    mov a, $c3
    beq short_return_guard_bad
short_return_guard_done:
    ret
short_return_guard_bad:
    jmp pair_reject
