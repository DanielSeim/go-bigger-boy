; SPDX-License-Identifier: GPL-3.0-or-later
; D5-only. Command 4 revokes the current generation after host preflight failure.
; Invalid uploads retain cooperative loader ownership; no ready advertisement.
recovery_dispatch:
    mov a, $f7
    cmp a, #$04
    bne recovery_dispatch_return
    mov a, #$01
    call bridge_observe
    call atomic_upload_begin
    mov a, $d0
    mov $f4, a
    jmp recovery_bad
recovery_dispatch_return:
    ret
recovery_bad:
    call bridge_mute
    ; Consume the IPL jump token (or rejected command) before cooperative polling.
    ; Its retained high restart-address byte is not a fresh command 4.
    mov a, $f4
    mov $d0, a
    mov $d1, #$00
    mov $d2, #$00
    mov $d8, #$00
    mov $db, #$00
    mov $dc, #$00
    mov $f5, #$d5
    mov $f6, #$e2
    mov $f7, #$a5
    .byte $cd, $ef
    .byte $bd
    jmp bridge_poll
recovery_stop:
    mov a, $d2
    beq recovery_stop_blocked
    mov $d2, #$01
    mov $f6, #$01
    ret
recovery_stop_blocked:
    mov $f6, #$e2
    ret
recovery_blocked_ack:
    mov a, $d0
    mov $f4, a
    ret
