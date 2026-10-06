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
    stz $24
    stz $25
    stz $26
    stz $27
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
; External-driver diagnostics observe its output only; never send mailbox data.
    lda $24
    beq poll_packets
    lda $2142
    sta $28
    lda $2143
    sta $29
poll_packets:
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
    cmp #$49
    bne not_transfer
    jmp transfer
not_transfer:
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
    lda $24
    beq own_sound
    jmp unsupported_owned
own_sound:
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
    jmp poll
unsupported:
; Diagnostic $21 holds the unsupported command header; $20=FF means halted.
; Silence both prototype voices before stopping packet consumption.
    lda $0100
    sta $21
    lda $24
    bne unsupported_owned
    lda #$80
    sta $2141
    sta $2142
    inc $23
    lda $23
    sta $2140
stop_ack:
    cmp $2140
    bne stop_ack
unsupported_owned:
    lda $0100
    sta $21
    lda #$ff
    sta $20
halt:
    bra halt


; SOU_TRN captures the first 256 tiles of the next complete GB frame.
; WRAM $1000..1FFF is private latch storage. DMA reads actual arrived rows;
; no GB VRAM access or synthesized/future video bytes occur here.
transfer:
    lda $24
    beq own_transfer
    jmp unsupported_owned
own_transfer:
    lda #$03
    sta $20
    stz $27
    stz $2181
    lda #$10
    sta $2182
    stz $2183
    lda #$08
    sta $4300
    lda #$80
    sta $4301
    stz $4302
    lda #$78
    sta $4303
    stz $4304
    ldy #$3fff
wait_vblank:
    lda $6000
    and #$f8
    cmp #$88
    beq await_frame
    dey
    bne wait_vblank
    jmp row_timeout
await_frame:
    ldy #$3fff
wait_frame:
    lda $6000
    and #$f8
    beq capture_setup
    dey
    bne wait_frame
    jmp row_timeout
capture_setup:
    ldx #$0000
    lda #$08
    sta $30
capture_row:
    ldy #$3fff
row_wait:
    lda $6000
    and #$f8
    cmp $30
    beq row_ready
    bcc row_early
    jmp row_timeout
row_early:
    dey
    bne row_wait
    jmp row_timeout
row_ready:
    txa
    and #$03
    sta $6001
    lda #$01
    sta $4306
    cpx #$000c
    beq last_row
    lda #$40
    bra row_size
last_row:
    lda #$00
row_size:
    sta $4305
    lda #$01
    sta $420b
    lda $30
    clc
    adc #$08
    sta $30
    inx
    cpx #$000d
    bne capture_row
    inc $25
; Validate the entire list before the SPC ownership release or any upload.
    ldx #$0000
validate_header:
    cpx #$0ffd
    bcc header_available
    jmp invalid_list
header_available:
    ldy $1000,x
    sty $40
    inx
    inx
    ldy $1000,x
    sty $42
    inx
    inx
    rep #$20
    lda $40
    beq validate_jump
    txa
    clc
    adc $40
    bcc source_no_wrap
    jmp invalid_list_wide
source_no_wrap:
    cmp.w #$1001
    bcc source_valid
    jmp invalid_list_wide
source_valid:
    tax
    lda $42
    cmp.w #$0100
    bcs destination_low_valid
    jmp reserved_range_wide
destination_low_valid:
    clc
    adc $40
    bcc destination_valid
    beq destination_valid
    jmp invalid_list_wide
destination_valid:
    sep #$20
    jmp validate_header
validate_jump:
    lda $42
    cmp.w #$0100
    bcs jump_low_valid
    jmp reserved_range_wide
jump_low_valid:
    cmp.w #$ffc0
    bcc jump_valid
    jmp reserved_range_wide
jump_valid:
    sta $44
    sep #$20
; Our cooperative driver enters IPL only after the complete payload is valid.
    lda #$04
    sta $20
    lda #$01
    sta $2143
    inc $23
    lda $23
    sta $2140
    ldy #$3fff
loader_wait:
    lda $2140
    cmp #$aa
    bne loader_retry
    lda $2141
    cmp #$bb
    beq loader_ready
loader_retry:
    dey
    bne loader_wait
    lda #$05
    jmp transfer_error
loader_ready:
    lda #$cc
    sta $47
    ldx #$0000
transfer_header:
    ldy $1000,x
    sty $40
    inx
    inx
    ldy $1000,x
    sty $42
    inx
    inx
    lda $42
    sta $2142
    lda $43
    sta $2143
    lda $40
    ora $41
    beq transfer_jump
    lda #$01
    sta $2141
    lda $47
    sta $2140
    jsr wait_echo
    stx $48
    ldy #$0000
transfer_byte:
    lda $1000,x
    sta $2141
    tya
    sta $2140
    jsr wait_echo
    inx
    iny
    sty $4a
    lda $4a
    sta $47
    lda $4b
    cmp $41
    bne transfer_byte
    lda $4a
    cmp $40
    bne transfer_byte
; Last byte counter N-1; an odd N+2 token is forward and never zero.
; Command echo zero would alias byte zero before the IPL stores it.
    lda $47
    clc
    adc #$02
    ora #$01
    sta $47
    jmp transfer_header
transfer_jump:
    stz $2141
    lda $47
    sta $2140
    jsr wait_echo
    inc $26
    lda #$01
    sta $24
    lda #$02
    sta $20
    jmp poll
wait_echo:
; Each upload acknowledgment is bounded. Preserve payload index Y.
    stx $4c
    ldx #$ffff
    sta $4e
echo_poll:
    cmp $2140
    beq echo_done
    dex
    bne echo_poll
    lda #$05
    jmp transfer_error
echo_done:
    ldx $4c
    rts
row_timeout:
    lda #$04
    jmp transfer_error
invalid_list_wide:
    sep #$20
invalid_list:
    lda #$01
    jmp transfer_error
reserved_range_wide:
    sep #$20
    lda #$03
transfer_error:
    sta $27
    lda $20
    cmp #$03
    beq error_still_owned
; Once IPL loading starts, our former driver cannot receive stop commands.
    lda #$01
    sta $24
    jmp unsupported_owned
error_still_owned:
    jmp unsupported
