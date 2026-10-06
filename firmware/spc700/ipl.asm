; SPDX-License-Identifier: GPL-3.0-or-later
; Original GBB port-protocol loader. Entry is $FFC0; 64-byte overlay.
; No reference image is used to assemble this source.
; Clear $EF..$01, leaving $00 and I/O untouched. Stack is $01EF.
; GBB resets PSW.P to zero before entering the image.
    mov a, #$00
    mov x, #$ef
    mov sp, x
clear:
    mov (x), a
    dec x
    bne clear
    mov $f4, #$aa
    mov $f5, #$bb
kick:
    cmp $f4, #$cc
    bne kick
    bra command
; Wait for index zero; the acknowledged command token is still incoming.
first_byte:
    mov y, $f4
    bne first_byte
poll:
    cmp y, $f4
    bne changed
receive:
    mov a, $f5
    mov $f4, y
    mov [$00]+y, a
    inc y
    bne poll
    inc $01
changed:
; Expected minus incoming is negative for a forward command token.
; The previous byte's token (including FF->00 wrap) keeps us waiting.
; This branch also handles the sign of the page increment at a byte wrap.
    bpl poll
; Upper RAM pages use a fresh counter comparison at the wrap boundary.
; Its sign belongs to the port token, rather than the pointer high byte.
    cmp y, $f4
    bpl poll
; Command address precedes acknowledgment. A nonzero mode starts a block.
command:
    movw ya, $f6
    movw $00, ya
; Latch both token and mode before publishing the acknowledgment: the host
; may put the first data byte on port 1 as soon as it sees that echo.
    movw ya, $f4
    mov $f4, a
    mov a, y
    mov x, a
    bne first_byte
execute:
; Match the useful execution contract: A=X=Y=0, SP=EF, entry in $00/$01.
    jmp [$0000+x]
