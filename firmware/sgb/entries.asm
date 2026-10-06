; SPDX-License-Identifier: GPL-3.0-or-later
; Original resident entry layout. Legacy $0200 restarts mailbox-v4 code at $1000.
    jmp $1000

; Uploaded N-SPC score restart is reserved but playback is not implemented yet.
; Fail silent, withdraw readiness, report E1 on output port 2, and remain external.
; Never advertise mailbox ownership merely because an upload jumps here.
    .org $0400
    mov $f4, #$00
    mov $f5, #$00
    mov $f6, #$e1
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
