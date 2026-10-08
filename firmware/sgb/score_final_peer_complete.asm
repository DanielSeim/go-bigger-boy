; SPDX-License-Identifier: GPL-3.0-or-later
final_peer_complete:
    mov $70, #$0c
    mov $71, #$00
    mov $f2, #$5c
    mov $f3, #$ff
    ; Reuse the existing hold across more than two DSP frames so an already
    ; sounding voice actually latches KOF before the register is cleared.
    call duet_wait
    mov $f3, #$00
    mov $f2, #$4c
    mov $f3, #$00
    ret
