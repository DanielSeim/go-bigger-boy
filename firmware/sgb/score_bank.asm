; SPDX-License-Identifier: GPL-3.0-or-later
; Owned bounded 16-bit bank reads. Source $2B00..32FF, length $20/$8E LE.
; Cursor $21/$90; phrase/table high $91/$92; call target/return high $93/$94.
; End $95/$96; indirect read scratch $97/$98; read budget $99/$9A.
; Runtime caches and event log occupy $4000..48FF, outside uploaded source.
bank_init:
    mov a, $8e
    cmp a, #$08
    bcc bank_length_ok
    bne bank_init_bad
    mov a, $20
    bne bank_init_bad
bank_length_ok:
    mov a, $20
    or a, $8e
    beq bank_init_bad
    mov a, $20
    mov $95, a
    mov a, $8e
    clrc
    adc a, #$2b
    mov $96, a
    mov $99, #$00
    mov $9a, #$00
    ret
bank_init_bad:
    jmp pair_reject
bank_read:
    mov a, $90
    cmp a, $96
    bcc bank_read_bounded
    bne bank_read_bad
    mov a, $21
    cmp a, $95
    bcs bank_read_bad
bank_read_bounded:
    ; At most 8192 successful byte reads across complete native expansion.
    inc $99
    mov a, $99
    bne bank_budget_check
    inc $9a
bank_budget_check:
    mov a, $9a
    cmp a, #$20
    bcc bank_read_load
    bne bank_read_bad
    mov a, $99
    bne bank_read_bad
bank_read_load:
    mov a, $21
    mov $97, a
    mov a, $90
    mov $98, a
    mov a, #$00
    mov x, a
    ; MOV A,[$97+X]: an absolute source address, not a page-relative index.
    .byte $e7, $97
    inc $21
    bne bank_read_done
    inc $90
bank_read_done:
    cmp a, #$00
    ret
bank_read_bad:
    jmp pair_reject
bank_pointer:
    mov a, $42
    cmp a, #$2b
    bcc bank_pointer_bad
    cmp a, $96
    bcc bank_pointer_ok
    bne bank_pointer_bad
    mov a, $41
    cmp a, $95
    bcs bank_pointer_bad
bank_pointer_ok:
    mov a, $41
    mov $21, a
    mov a, $42
    mov $90, a
    ret
bank_pointer_bad:
    jmp pair_reject
