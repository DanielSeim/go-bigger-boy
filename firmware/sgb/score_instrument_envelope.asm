; SPDX-License-Identifier: GPL-3.0-or-later
; D8-only exact additional ADSR descriptor. ADSR1 was checked as $8E.
; No new cross-combinations or arbitrary uploaded envelope fields.
envelope_validate:
    mov a, $5002+x
    cmp a, #$af
    bne envelope_bad
    mov a, $5003+x
    cmp a, #$b8
    bne envelope_bad
    ret
envelope_bad:
    jmp bridge_bad
