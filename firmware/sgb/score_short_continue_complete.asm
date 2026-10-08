; SPDX-License-Identifier: GPL-3.0-or-later
short_continue_complete:
    mov a, $c5
    beq short_continue_complete_old
    mov $51, #$00
    jmp final_ready
short_continue_complete_old:
    jmp final_complete
