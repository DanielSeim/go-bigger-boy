#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an opt-in real-upload/restart/selection ROM around the native engine."""
import argparse
import hashlib
import re
from pathlib import Path
from build_sgb_prototype import assemble, ROOT
from build_sgb_score_short_pair import source as engine_source


def build():
    engine = engine_source()
    hooks = {
        'clock_poll:\n    mov a, $fd\n': 'clock_poll:\n    call bridge_service\n',
        '    call poly_setup\n':'    call bridge_start\n',
        '    mov $f1, #$80\n    mov $14, #$02\nmulti_halted:\n    bra multi_halted\n':'multi_halted:\n    jmp bridge_complete\n',
        'pair_rejected:\n    bra pair_rejected\n':'pair_rejected:\n    jmp bridge_bad\n',
    }
    for before, after in hooks.items():
        if engine.count(before) != 1:
            raise ValueError('transport engine hook changed: ' + before)
        engine = engine.replace(before, after)
    bridge = (ROOT / 'firmware/sgb/score_transport_bridge.asm').read_text()
    branch_number = 0
    def bridge_branch(match):
        nonlocal branch_number
        branch_number += 1
        inverse = {'bne': 'beq', 'beq': 'bne'}[match[1]]
        label = f'bridge_branch_{branch_number}'
        return f'    {inverse} {label}\n    jmp {match[2]}\n{label}:\n'
    bridge = re.sub(r'    (bne|beq) (bridge_bad|bridge_loader|bridge_stop)\n', bridge_branch, bridge)
    payload = bytes(assemble(bridge + '\n' + engine, 'spc', 0x0200))
    if len(payload) > 0x1600:
        raise ValueError('transport payload exceeds bridge/engine region')
    host = (ROOT / 'firmware/sgb/host.asm').read_text()
    hooks = {
        '    cmp #$c4\n    bne driver_ready\n':'    cmp #$cb\n    bne driver_ready\n',
        '    cmp #$ca\n    bne poll_packets\n':'    cmp #$ca\n    beq driver_version_known\n    cmp #$cb\n    bne poll_packets\n',
        '    cmp #$ca\n    bne stop_control_ready\n': '    cmp #$ca\n    beq stop_all_control\n    cmp #$cb\n    bne stop_control_ready\n',
        'own_sound:\n':'own_sound:\n    lda $2b\n    cmp #$cb\n    bne bridge_host_old_sound\n    lda $0101\n    ora $0102\n    ora $0103\n    bne unsupported_early\n    lda $0104\n    cmp #$80\n    beq bridge_host_valid_score\n    cmp #$01\n    bne unsupported_early\nbridge_host_valid_score:\n    jmp send_bridge_score\nbridge_host_old_sound:\n',
        '    lda $0103\n    sta $2141\n':'send_bridge_score:\n    lda $0103\n    sta $2141\n',
        '\npoll:\n':'\npoll:\n    lda $2141\n    sta $30\n    lda $2142\n    sta $31\n    lda $2143\n    sta $32\n',
    }
    for before, after in hooks.items():
        if host.count(before) != 1:
            raise ValueError('transport host hook changed: ' + before)
        host = host.replace(before, after)
    # Expanded diagnostic guards need absolute failure jumps in the host variant.
    guard_number = 0
    def far_guard(match):
        nonlocal guard_number
        guard_number += 1
        inverse = {'bne': 'beq', 'beq': 'bne', 'bcs': 'bcc'}[match[1]]
        label = f'bridge_guard_{guard_number}'
        return f'    {inverse} {label}\n    jmp unsupported\n{label}:\n'
    host = re.sub(r'    (bne|beq|bcs) unsupported_early\n', far_guard, host)
    host = host.replace('    bra poll\n', '    jmp poll\n')
    host = bytes(assemble(host, 'host', 0x8000, {
        'payload_size': len(payload), 'entry_token': ((len(payload) + 2) | 1) & 255}))
    if len(host) > 4096:
        raise ValueError('transport host overlaps payload')
    rom = bytearray(0x40000)
    rom[:len(host)] = host
    rom[0x1000:0x1000 + len(payload)] = payload
    rom[0x7fc0:0x7fd5] = b'GBB SCORE TRANSPORT  '
    rom[0x7fd5:0x7fda] = bytes((0x20, 0, 8, 0, 1))
    rom[0x7ffc:0x8000] = bytes((0, 0x80, 0, 0x80))
    checksum = (sum(rom) + 510) & 65535
    rom[0x7fdc:0x7fe0] = ((checksum ^ 65535).to_bytes(2, 'little') +
                           checksum.to_bytes(2, 'little'))
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
