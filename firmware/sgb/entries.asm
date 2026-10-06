; SPDX-License-Identifier: GPL-3.0-or-later
; Original resident entry layout. Legacy $0200 restarts mailbox-v4 code at $1000.
    mov $30, #$00
    jmp $1000

; Uploaded original score restart validates the explicit GBS1 subset at $1400.
; Only complete validation may select v5. Rejection remains silent and external.
    .org $0400
    jmp $1400
    .org $0410
; Validator rejection entry, A contains diagnostic E1/E2.
    mov $f4, #$00
    mov $f5, #$00
    mov $f6, a
    mov $f7, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    mov $f2, #$4c
    mov $f3, #$00
    mov $f2, #$6c
    mov $f3, #$e0
    mov $f1, #$80
unsupported_score:
    bra unsupported_score
