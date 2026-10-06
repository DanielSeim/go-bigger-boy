; SPDX-License-Identifier: GPL-3.0-or-later
; Original experimental SGB host, LoROM bank 0, entry $8000.
; Native mode, 8-bit accumulator, 16-bit indexes. IRQs remain disabled.
    sei
    clc
    xce
    rep #$10
    stz $20
    stz $21
    stz $22
ready:
    lda $2140
    cmp #$aa
    bne ready
ready_high:
    lda $2141
    cmp #$bb
    bne ready_high
; Upload one contiguous original payload to $0200, using the IPL handshake.
    lda #$00
    sta $2142
    lda #$02
    sta $2143
    lda #$01
    sta $2141
    lda #$cc
    sta $2140
upload_command:
    cmp $2140
    bne upload_command
    ldx #$0000
upload:
    lda $9000,x
    sta $2141
    txa
    sta $2140
upload_ack:
    cmp $2140
    bne upload_ack
    inx
    cpx #payload_size
    bne upload
; Mode zero jumps to the driver. Advance beyond the previous byte token.
    lda #$00
    sta $2142
    sta $2141
    lda #$02
    sta $2143
    lda #entry_token
    sta $2140
entry_ack:
    cmp $2140
    bne entry_ack
; Driver publishes a separate ready signature after DSP setup.
driver_ready:
    lda $2143
    cmp #$a5
    bne driver_ready
    lda $2140
    cmp #$5a
    bne driver_ready
    stz $23
    lda #$01
    sta $20
; Both models use their existing ICD oscillator profiles, divider 5.
    lda #$81
    sta $6003
poll:
    lda $6002
    beq poll
; $7000 pops a packet and latches all sixteen bytes. Copy them exactly once.
    ldx #$0000
packet:
    lda $7000,x
    sta $0100,x
    inx
    cpx #$0010
    bne packet
    inc $22
    lda $0100
    cmp #$41
    beq sound
; Recognize SOUND framing errors and SOU_TRN as unsupported, never ignore them.
    and #$f8
    cmp #$40
    beq unsupported
    cmp #$48
    beq unsupported
; Other commands remain handled by the GB display's existing SGB adapter.
    bra poll
sound:
; Prototype accepts unchanged attributes and no music score only.
    lda $0103
    ora $0104
    bne unsupported
    lda $0101
    cmp #$80
    beq valid_a
    cmp #$02
    bcs unsupported
valid_a:
    sta $2141
    lda $0102
    cmp #$80
    beq valid_b
    cmp #$02
    bcs unsupported
valid_b:
    sta $2142
    inc $23
    lda $23
    sta $2140
sound_ack:
    cmp $2140
    bne sound_ack
    bra poll
unsupported:
; Diagnostic $21 holds the unsupported command header; $20=FF means halted.
; Silence both prototype voices before stopping packet consumption.
    lda $0100
    sta $21
    lda #$80
    sta $2141
    sta $2142
    inc $23
    lda $23
    sta $2140
stop_ack:
    cmp $2140
    bne stop_ack
    lda #$ff
    sta $20
halt:
    bra halt
