; SPDX-License-Identifier: GPL-3.0-or-later
; Native lookup of measured instrument-2 pitches for base notes 24..36.
; The independently authored waveform/routing/envelope remain diagnostic.
duet_voice:
    mov a, $26
    cmp a, #$c9
    beq duet_voice_done
    ; Validated opcodes $98..A4 index parallel low/high tables directly.
    mov x, a
    ; $0EA8 + $98 = $0F40; $0EB8 + $98 = $0F50.
    mov a, $0ea8+x
    mov $50, a
    mov a, $0eb8+x
    mov x, a
    ; Continue through the existing independent per-voice pitch/KON writer.
