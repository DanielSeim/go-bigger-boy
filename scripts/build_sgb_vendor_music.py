#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an opt-in single-channel vendor-format music experiment with owned assets."""
import argparse
import hashlib
import re
from pathlib import Path
from build_sgb_prototype import assemble, ROOT


def long_branches(source):
    """Keep independently authored guard paths independent of branch distances."""
    sequence = 0
    def expand(match):
        nonlocal sequence
        sequence += 1
        inverse = {'beq': 'bne', 'bne': 'beq', 'bcc': 'bcs', 'bcs': 'bcc'}[match[1]]
        label = f'vendor_guard_{sequence}'
        return f'    {inverse} {label}\n    jmp {match[2]}\n{label}:\n'
    return re.sub(r'    (beq|bne|bcc|bcs) (vendor_[a-z_]+)\n', expand, source)


def replace(source, old, new):
    if source.count(old) != 1:
        raise ValueError('vendor host hook changed: ' + old)
    return source.replace(old, new)


def build():
    source = (ROOT / 'firmware/sgb/vendor_music.asm').read_text()
    source = replace(source, '    mov x, #$00\n', '    .byte $cd, $00\n')
    pitches = [min(16383, round(1068 * 2 ** ((note - 24) / 12))) for note in range(72)]
    # Limited register observations from owned note fixtures, not a resident
    # table extraction or a pitch law outside these fourteen notes.
    measured = (1068,1132,1200,1272,1348,1428,1512,1604,1700,1800,1908,2020,2140,2268)
    source = replace(source, 'vendor_measured_pitch:\n', 'vendor_measured_pitch:\n.byte ' +
        ','.join(f'${byte:02x}' for pitch in measured for byte in pitch.to_bytes(2,'little')) + '\n')
    source += '.byte ' + ','.join(f'${byte:02x}' for pitch in pitches for byte in pitch.to_bytes(2, 'little')) + '\n'
    payload = assemble(long_branches(source), 'spc', 0x0200)
    if len(payload) > 0x1900:
        raise ValueError('vendor payload overlaps reserved score space')
    host = (ROOT / 'firmware/sgb/host.asm').read_text()
    host = replace(host, '    stz $2a\n', '    stz $2a\n    stz $5a\n    stz $5b\n    stz $60\n    stz $61\n    stz $62\n')
    host = replace(host, '    cmp #$c4\n    bne driver_ready\n', '    cmp #$db\n    bne driver_ready\n')
    host = replace(host, '    cmp #$ca\n    bne poll_packets\n',
                   '    cmp #$ca\n    beq driver_version_known\n    cmp #$db\n    bne poll_packets\n')
    host = replace(host, '    cmp #$ca\n    bne stop_control_ready\n',
                   '    cmp #$ca\n    beq stop_all_control\n    cmp #$db\n    bne stop_control_ready\n')
    host = replace(host, 'own_sound:\n', '''own_sound:
    lda $0101
    beq vendor_effect_b
    cmp #$80
    bne unsupported_early
vendor_effect_b:
    lda $0102
    beq vendor_attributes
    cmp #$80
    bne unsupported_early
vendor_attributes:
    lda $0103
    and #$c0
    cmp #$c0
    beq unsupported_early
    lda $0104
    cmp #$80
    beq vendor_send_score
    cmp #$04
    bcs unsupported_early
vendor_send_score:
    jmp vendor_host_send
''')
    host = replace(host, '    lda $0103\n    sta $2141\n', 'vendor_host_send:\n    lda $0103\n    sta $2141\n')
    host = replace(host, 'send_effects:\n    lda $0101\n    sta $2141\n    lda $0102\n    sta $2142\n',
                   'send_effects:\n    stz $2141\n    stz $2142\n')
    host = replace(host, '\npoll:\n', '''
poll:
    lda $2142
    cmp #$e2
    bne vendor_host_poll
    jmp unsupported_owned
vendor_host_poll:
''')
    host = replace(host, '    inc $25\n',
                   '    inc $25\n    stz $54\n    stz $5c\n    rep #$20\n    lda.w #$2b00\n    sta $52\n    lda.w #$3b00\n    sta $5e\n    sep #$20\n')
    host = replace(host, 'destination_valid:\n    sep #$20',
                   'destination_valid:\n    jsr vendor_block\n    sep #$20')
    host = replace(host, 'jump_valid:\n    sta $44', 'jump_valid:\n    jsr vendor_complete\n    sta $44')
    host = replace(host, 'transfer_jump:\n    stz $2141\n', '''transfer_jump:
; Supply a trusted exclusive source end, outside the score/code/sample regions.
; The game cannot upload this descriptor or resident code.
    lda #$10
    sta $2142
    lda #$05
    sta $2143
    lda #$01
    sta $2141
    lda $47
    sta $2140
    jsr wait_echo
    lda $5a
    sta $2141
    stz $2140
    lda #$00
    jsr wait_echo
    lda $5b
    sta $2141
    lda #$01
    sta $2140
    jsr wait_echo
    lda $60
    sta $2141
    lda #$02
    sta $2140
    jsr wait_echo
    lda $62
    sta $2141
    lda #$03
    sta $2140
    jsr wait_echo
    lda $63
    sta $2141
    lda #$04
    sta $2140
    jsr wait_echo
    stz $2142
    lda #$04
    sta $2143
    lda #$07
    sta $47
    stz $2141
''')
    host += '''
vendor_block:
    lda $42
    cmp.w #$3b00
    bcs vendor_asset_block
    cmp $52
    bne vendor_invalid
    clc
    adc $40
    cmp.w #$3b01
    bcs vendor_invalid
    sta $52
    lda.w #$0001
    sta $54
    rts
vendor_asset_block:
    cmp.w #$4b08
    beq vendor_directory_block
    cmp.w #$4c3c
    beq vendor_descriptor_block
    cmp $5e
    bne vendor_invalid
    clc
    adc $40
    cmp.w #$4b01
    bcs vendor_invalid
    sta $5e
    lda $5c
    .byte $09, $01, $00
    sta $5c
    rts
vendor_directory_block:
    lda $40
    cmp.w #$0004
    bne vendor_invalid
    lda $5c
    .byte $29, $02, $00
    bne vendor_invalid
    lda $5c
    .byte $09, $02, $00
    sta $5c
    rts
vendor_descriptor_block:
    lda $40
    cmp.w #$0006
    bne vendor_invalid
    lda $5c
    .byte $29, $04, $00
    bne vendor_invalid
    lda $5c
    .byte $09, $04, $00
    sta $5c
    rts
vendor_complete:
    lda $42
    cmp.w #$0400
    bne vendor_invalid
    lda $54
    beq vendor_previous_bank
    lda $52
    cmp.w #$2b06
    bcc vendor_invalid
    sta $5a
    stz $60
    stz $61
vendor_previous_bank:
    lda $5a
    cmp.w #$2b06
    bcc vendor_invalid
    lda $5c
    beq vendor_manifest_done
    cmp.w #$0007
    bne vendor_invalid
    lda.w #$0001
    sta $60
    lda $5e
    sta $62
vendor_manifest_done:
    lda $42
    rts
vendor_invalid:
    jmp invalid_list_wide
'''
    # Newly added SOUND guards can be beyond the relative-branch range.
    guard = 0
    def far(match):
        nonlocal guard
        guard += 1
        inverse = {'bne': 'beq', 'beq': 'bne', 'bcs': 'bcc', 'bcc': 'bcs'}[match[1]]
        label = f'vendor_host_guard_{guard}'
        return f'    {inverse} {label}\n    jmp unsupported\n{label}:\n'
    host = re.sub(r'    (bne|beq|bcs|bcc) unsupported_early\n', far, host)
    host = re.sub(r'    (bne|beq|bcs|bcc) vendor_invalid\n', lambda m: far(m).replace('jmp unsupported', 'jmp vendor_invalid'), host)
    host = host.replace('    bra poll\n', '    jmp poll\n')
    host = assemble(host, 'host', 0x8000, {'payload_size': len(payload), 'entry_token': ((len(payload)+2)|1)&255})
    if len(host) > 4096:
        raise ValueError('vendor host overlaps payload')
    rom = bytearray(0x40000)
    rom[:len(host)] = host
    rom[0x1000:0x1000+len(payload)] = payload
    rom[0x7fc0:0x7fd5] = b'GBB VENDOR MUSIC EXP '
    rom[0x7fd5:0x7fda] = bytes((0x20, 0, 8, 0, 1))
    rom[0x7ffc:0x8000] = bytes((0, 0x80, 0, 0x80))
    checksum = (sum(rom) + 510) & 65535
    rom[0x7fdc:0x7fe0] = (checksum^65535).to_bytes(2, 'little') + checksum.to_bytes(2, 'little')
    return bytes(rom)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build()
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
