; SPDX-License-Identifier: GPL-3.0-or-later
; $9B event-log high byte; $9C live/rehearsal pattern index;
; $9D/9E rehearsal tick count; $9F byte scratch; $A1 log publication busy.
list_tick_budget:
    mov a, $64
    bne list_return
    inc $9d
    mov a, $9d
    bne list_ticks_check
    inc $9e
list_ticks_check:
    mov a, $9e
    cmp a, #$07
    bcc list_return
    bne list_runtime_bad
    mov a, $9d
    cmp a, #$f1
    bcs list_runtime_bad
list_return:
    ret
list_runtime_bad:
    jmp pair_reject
list_log_store:
    mov $9f, a
    mov x, $28
    mov a, $9b
    beq list_log_page0
    mov a, $9f
    .byte $d5, $00, $41
    bra list_log_increment
list_log_page0:
    mov a, $9f
    .byte $d5, $00, $40
list_log_increment:
    inc $28
    mov a, $28
    bne list_return
    inc $9b
    ret
