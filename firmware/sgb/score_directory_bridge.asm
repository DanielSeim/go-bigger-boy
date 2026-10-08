; SPDX-License-Identifier: GPL-3.0-or-later
; CC-only three-root directory. D8 = directory byte offset 0/2/4.
; D9/DA = current root low/high; DB = last rendered song, DC = admitted roots.
; Kept outside restart/archive and the fixed native engine allocation.
.org $0600
bridge_directory_cursor:
    mov a, $d8
    cmp a, #$00
    beq directory_index_ok
    cmp a, #$02
    beq directory_index_ok
    cmp a, #$04
    bne directory_bad
directory_index_ok:
    mov x, $d8
    mov a, $2b00+x
    mov $d9, a
    mov a, $2b01+x
    mov $da, a
    cmp a, #$2b
    bcc directory_bad
    cmp a, #$33
    bcs directory_bad
    cmp a, #$2b
    bne directory_upper
    mov a, $d9
    cmp a, #$06
    bcc directory_bad
directory_upper:
    mov a, $da
    cmp a, #$32
    bne directory_duplicates
    mov a, $d9
    cmp a, #$ff
    beq directory_bad
directory_duplicates:
    mov a, $d8
    beq directory_cursor
    mov a, $d9
    mov $df, a
    mov a, $2b00
    mov $dd, a
    mov a, $df
    cmp a, $dd
    bne directory_second
    mov a, $da
    mov $df, a
    mov a, $2b01
    mov $de, a
    mov a, $df
    cmp a, $de
    beq directory_bad
directory_second:
    mov a, $d8
    cmp a, #$04
    bne directory_cursor
    mov a, $d9
    mov $df, a
    mov a, $2b02
    mov $dd, a
    mov a, $df
    cmp a, $dd
    bne directory_cursor
    mov a, $da
    mov $df, a
    mov a, $2b03
    mov $de, a
    mov a, $df
    cmp a, $de
    beq directory_bad
directory_cursor:
    mov a, $d8
    mov $21, a
    ret
directory_bad:
    jmp bridge_bad
