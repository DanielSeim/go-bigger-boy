#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build an opt-in real-upload/restart/selection ROM around the native engine."""
import argparse
import hashlib
import re
from pathlib import Path
from build_sgb_prototype import assemble, ROOT
from build_sgb_score_short_pair import source as engine_source


def replace_exact(text, before, after, count=1):
    if text.count(before) != count:
        raise ValueError('directory bridge hook changed: ' + before)
    return text.replace(before, after)


def build(*, multisong=False, uploaded_instrument=False, two_instruments=False,
          multiblock=False, instrument_profiles=False, one_shot=False, brr_profiles=False, relocatable=False, atomic_upload=False, upload_recovery=False, instrument_mapping=False):
    if type(multisong) is not bool:
        raise ValueError("multisong flag must be boolean")
    if type(uploaded_instrument) is not bool or (uploaded_instrument and not multisong):
        raise ValueError("uploaded instrument requires a boolean flag and multisong")
    if type(two_instruments) is not bool or (two_instruments and not uploaded_instrument):
        raise ValueError("two instruments require a boolean flag and uploaded instrument")
    if type(multiblock) is not bool or (multiblock and not two_instruments):
        raise ValueError("multiblock requires a boolean flag and two instruments")
    if type(instrument_profiles) is not bool or (instrument_profiles and not multiblock):
        raise ValueError("instrument profiles require a boolean flag and multiblock")
    if type(one_shot) is not bool or (one_shot and not instrument_profiles):
        raise ValueError("one-shot requires a boolean flag and instrument profiles")
    if type(brr_profiles) is not bool or (brr_profiles and not one_shot):
        raise ValueError("BRR profiles require a boolean flag and one-shot")
    if type(relocatable) is not bool or (relocatable and not brr_profiles):
        raise ValueError("relocatable samples require a boolean flag and BRR profiles")
    if type(atomic_upload) is not bool or (atomic_upload and not relocatable):
        raise ValueError("atomic upload requires a boolean flag and relocatable samples")
    if type(upload_recovery) is not bool or (upload_recovery and not atomic_upload):
        raise ValueError("upload recovery requires a boolean flag and atomic upload")
    if type(instrument_mapping) is not bool or (instrument_mapping and not upload_recovery):
        raise ValueError("instrument mapping requires a boolean flag and upload recovery")
    engine = engine_source()
    hooks = {
        'clock_poll:\n    mov a, $fd\n': 'clock_poll:\n    call bridge_service\n',
        '    call poly_setup\n':'    call bridge_start\n',
        '    mov $f1, #$80\n    mov $14, #$02\nmulti_halted:\n    bra multi_halted\n':'multi_halted:\n    jmp bridge_complete\n',
        'pair_rejected:\n    bra pair_rejected\n':'pair_rejected:\n    jmp bridge_bad\n',
    }
    if multisong:
        hooks['    call timing_init\n    mov $21, #$00\n'] = '    call timing_init\n    call bridge_directory_cursor\n'
    for before, after in hooks.items():
        if engine.count(before) != 1:
            raise ValueError('transport engine hook changed: ' + before)
        engine = engine.replace(before, after)
    bridge = (ROOT / 'firmware/sgb/score_transport_bridge.asm').read_text()
    if multisong:
        bridge = replace_exact(bridge, 'mov $f5, #$cb', 'mov $f5, #$cc')
        bridge = replace_exact(bridge, '    mov $d1, #$00\n    mov $f6, #$03',
            '    mov a, $d1\n    clrc\n    adc a, $d1\n    mov $d8, a\n    dec $d8\n    dec $d8\n    mov a, $d1\n    mov $db, a\n    mov $d1, #$00\n    mov $f6, #$03')
        bridge = replace_exact(bridge, '    cmp a, #$01\n    bne bridge_bad\n    mov $d1, #$01',
            '    cmp a, #$01\n    beq directory_stage_ok\n    cmp a, #$02\n    beq directory_stage_ok\n    cmp a, #$03\n    bne bridge_bad\ndirectory_stage_ok:\n    mov $d1, a')
        bridge = replace_exact(bridge, '    mov $d2, #$00\n',
            '    mov $d2, #$00\n    mov $d8, #$00\n    mov $db, #$00\n    mov $dc, #$00\n', count=2)
        bridge = replace_exact(bridge, '    mov $d2, #$01\n    jmp bridge_publish',
            '    inc $dc\n    mov a, $d8\n    cmp a, #$04\n    beq directory_validated\n    inc $d8\n    inc $d8\n    .byte $cd, $ef\n    .byte $bd\n    jmp $0800\ndirectory_validated:\n    mov $d8, #$00\n    mov $d2, #$01\n    jmp bridge_publish')
        prefix, handlers = bridge.split('; Diagnostic observations:', 1)
        handlers, restart = handlers.split('.org $0400', 1)
        bridge = prefix + '.org $0400' + restart + '\n.org $0508\n; Diagnostic observations:' + handlers
        bridge += '\n' + (ROOT / 'firmware/sgb/score_directory_bridge.asm').read_text()
    if uploaded_instrument:
        engine = replace_exact(engine, '    call timing_init\n', '    call uploaded_timing_init\n')
        engine = replace_exact(engine, '    mov a, owned_instrument_descriptor+x\n', '    mov a, $5000+x\n')
        bridge = replace_exact(bridge, 'mov $f5, #$cc', 'mov $f5, #$cd')
        bridge = replace_exact(bridge, '    call poly_setup\n', '    call uploaded_setup\n')
    if two_instruments:
        bridge = replace_exact(bridge, 'mov $f5, #$cd', 'mov $f5, #$ce')
        engine = replace_exact(engine, '    cmp a, #$ed\n    beq mix_control_dispatch\n',
            '    cmp a, #$ed\n    beq mix_control_dispatch\n    cmp a, #$e0\n    beq mix_control_dispatch\n')
        begin = engine.index('reselect_instrument:\n')
        end = engine.index('reselect_cache:\n', begin)
        engine = engine[:begin] + 'reselect_instrument:\n    jmp dual_instrument\n' + engine[end:]
        begin = engine.index('reselect_field:\n')
        end = engine.index('reselect_pending:\n', begin)
        engine = engine[:begin] + 'reselect_field:\n    jmp dual_field\n' + engine[end:]
        engine = replace_exact(engine, 'calls_track_end:\n    call mix_cache\n',
            'calls_track_end:\n    call dual_end_cache\n')
        engine = replace_exact(engine, 'tail_cursor:\n    mov $63, a\n',
            'tail_cursor:\n    mov $63, a\n    call reselect_event\n')
        engine = replace_exact(engine, 'reselect_event:\n', 'reselect_event:\n    mov $e6, #$00\n')
        engine = replace_exact(engine, 'reselect_write:\n', 'reselect_write:\n    call dual_descriptor\n')
    if multiblock:
        bridge = replace_exact(bridge, 'mov $f5, #$ce', 'mov $f5, #$cf')
    if instrument_profiles:
        bridge = replace_exact(bridge, 'mov $f5, #$cf', 'mov $f5, #$d0')
        engine = replace_exact(engine, 'duet_pitch:\n', 'duet_pitch:\n    call profile_pitch\n')
    if one_shot:
        bridge = replace_exact(bridge, 'mov $f5, #$d0', 'mov $f5, #$d1')
    if brr_profiles:
        bridge = replace_exact(bridge, 'mov $f5, #$d1', 'mov $f5, #$d2')
    if relocatable:
        bridge = replace_exact(bridge, 'mov $f5, #$d2', 'mov $f5, #$d3')
    if atomic_upload:
        bridge = replace_exact(bridge, 'mov $f5, #$d3', 'mov $f5, #$d4')
        bridge = replace_exact(bridge, '    call bridge_save_observations\n',
                               '    call atomic_upload_begin\n')
        bridge = replace_exact(bridge, 'directory_validated:\n    mov $d8, #$00\n    mov $d2, #$01',
                               'directory_validated:\n    mov $d8, #$00\n    call atomic_publish')
    if upload_recovery:
        bridge = replace_exact(bridge, 'mov $f5, #$d4', 'mov $f5, #$d5')
        bridge = replace_exact(bridge, '    mov $d0, a\n',
                               '    mov $d0, a\n    call recovery_dispatch\n')
        bridge = replace_exact(bridge, 'bridge_bad:\n    mov $f5, #$00',
                               'bridge_bad:\n    jmp recovery_bad')
        bridge = replace_exact(bridge, 'bridge_stage:\n    mov a, $d2\n    beq bridge_bad',
                               'bridge_stage:\n    mov a, $d2\n    beq recovery_blocked_ack')
        bridge = replace_exact(bridge, '    mov $d2, #$01\n    mov $14, #$02\n    mov $f6, #$01',
                               '    call recovery_stop\n    mov $14, #$02')
    if instrument_mapping:
        bridge = replace_exact(bridge, 'mov $f5, #$d5', 'mov $f5, #$d6')
    branch_number = 0
    def bridge_branch(match):
        nonlocal branch_number
        branch_number += 1
        inverse = {'bne': 'beq', 'beq': 'bne', 'bcc': 'bcs', 'bcs': 'bcc'}[match[1]]
        label = f'bridge_branch_{branch_number}'
        return f'    {inverse} {label}\n    jmp {match[2]}\n{label}:\n'
    targets = 'bridge_bad|bridge_loader|bridge_stop'
    if multisong:
        targets += '|bridge_no_command|bridge_ack'
    if upload_recovery:
        targets += '|recovery_blocked_ack'
    bridge = re.sub(r'    (bne|beq) (' + targets + r')\n', bridge_branch, bridge)
    if multisong:
        bridge = re.sub(r'    (bne|beq|bcc|bcs) (directory_bad)\n', bridge_branch, bridge)
    source = bridge + '\n' + engine
    if uploaded_instrument:
        helper = (ROOT / ('firmware/sgb/score_dual_instrument.asm' if two_instruments else
                          'firmware/sgb/score_uploaded_instrument.asm')).read_text()
        if multiblock:
            runtime = helper[helper.index('uploaded_setup:'):helper.index('dual_structure:')]
            helper = (ROOT / 'firmware/sgb/score_brr_chain.asm').read_text() + '\n' + runtime
            if instrument_profiles:
                helper = (ROOT / ('firmware/sgb/score_relocated_samples.asm' if relocatable else
                                  'firmware/sgb/score_brr_profiles.asm' if brr_profiles else
                                  'firmware/sgb/score_one_shot.asm' if one_shot else
                                  'firmware/sgb/score_instrument_profiles.asm')).read_text()
            if instrument_mapping:
                helper = replace_exact(helper, 'uploaded_timing_init:\n',
                                       'uploaded_timing_init:\n    call mapping_validate\n')
                helper = replace_exact(helper, '    cmp x, #$17\n    beq profile_reserved_next\n',
                    '    cmp x, #$17\n    beq profile_reserved_next\n'
                    '    cmp x, #$18\n    beq profile_reserved_next\n'
                    '    cmp x, #$19\n    beq profile_reserved_next\n')
                helper = replace_exact(helper,
                    'dual_instrument:\n    mov a, $4d\n    cmp a, #$02\n    beq dual_id_ok\n'
                    '    cmp a, #$03\n    beq dual_id_ok\n    jmp pair_reject\n',
                    'dual_instrument:\n    mov a, $4d\n    call mapping_lookup\n    mov $4d, a\n')
            helper = re.sub(r'    (bne|beq|bcc|bcs) (brr_bad)\n', bridge_branch, helper)
        if atomic_upload:
            helper += '\n' + (ROOT / 'firmware/sgb/score_atomic_upload.asm').read_text()
        if upload_recovery:
            recovery = (ROOT / 'firmware/sgb/score_upload_recovery.asm').read_text()
            if instrument_mapping:
                recovery = replace_exact(recovery, 'mov $f5, #$d5', 'mov $f5, #$d6')
            helper += '\n' + recovery
        if instrument_mapping:
            helper += '\n' + (ROOT / 'firmware/sgb/score_instrument_mapping.asm').read_text()
        source += '\n' + helper
    payload = bytes(assemble(source, 'spc', 0x0200))
    if len(payload) > (0x1a00 if uploaded_instrument else 0x1600):
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
    if multisong:
        host = replace_exact(host, 'cmp #$cb', 'cmp #$cc', count=4)
        host = replace_exact(host, '    cmp #$01\n    bne unsupported_early\nbridge_host_valid_score:',
            '    cmp #$01\n    bcc unsupported_early\n    cmp #$04\n    bcs unsupported_early\nbridge_host_valid_score:')
    if uploaded_instrument:
        host = replace_exact(host, 'cmp #$cc', 'cmp #$cd', count=4)
    if two_instruments:
        host = replace_exact(host, 'cmp #$cd', 'cmp #$ce', count=4)
    if multiblock:
        host = replace_exact(host, 'cmp #$ce', 'cmp #$cf', count=4)
    if instrument_profiles:
        host = replace_exact(host, 'cmp #$cf', 'cmp #$d0', count=4)
    if one_shot:
        host = replace_exact(host, 'cmp #$d0', 'cmp #$d1', count=4)
    if brr_profiles:
        host = replace_exact(host, 'cmp #$d1', 'cmp #$d2', count=4)
    if relocatable:
        host = replace_exact(host, 'cmp #$d2', 'cmp #$d3', count=4)
    if atomic_upload:
        host = replace_exact(host, 'cmp #$d3', 'cmp #$d4', count=4)
        host = replace_exact(host, '    inc $25\n',
            '    inc $25\n    rep #$20\n    lda.w #$2b00\n    sta $52\n    lda.w #$5000\n    sta $54\n    sep #$20\n')
        host = replace_exact(host, 'destination_valid:\n    sep #$20',
                             'destination_valid:\n    jsr atomic_block\n    sep #$20')
        host = replace_exact(host, 'jump_valid:\n    sta $44',
                             'jump_valid:\n    jsr atomic_complete\n    sta $44')
        host += '\n' + (ROOT / 'firmware/sgb/host_atomic_upload.asm').read_text()
        host = replace_exact(host, 'unsupported_owned:\n',
            'unsupported_owned:\n    lda $2141\n    sta $30\n    lda $2142\n    sta $31\n    lda $2143\n    sta $32\n')
    if upload_recovery:
        host = replace_exact(host, 'cmp #$d4', 'cmp #$d5', count=4)
        host = replace_exact(host, '    stz $2a\nready:',
                             '    stz $2a\n    stz $56\n    stz $57\n    stz $59\nready:')
        host = replace_exact(host, '    sta $32\n; Observe external drivers.',
                             '    sta $32\n    jsr recovery_poll\n; Observe external drivers.')
        host = replace_exact(host, 'sound:\n    lda $24',
            'sound:\n    jsr recovery_sound_check\n    bcc recovery_sound_pass\n    jmp poll\nrecovery_sound_pass:\n    lda $24')
        host = replace_exact(host, '    stz $24\n    inc $2a',
                             '    stz $24\n    stz $56\n    inc $2a')
        host = replace_exact(host, 'error_still_owned:\n    jmp unsupported',
                             'error_still_owned:\n    jmp recovery_reject')
        host += '\n' + (ROOT / 'firmware/sgb/host_upload_recovery.asm').read_text()
    if instrument_mapping:
        host = replace_exact(host, 'cmp #$d5', 'cmp #$d6', count=5)
    # Expanded diagnostic guards need absolute failure jumps in the host variant.
    guard_number = 0
    def far_guard(match):
        nonlocal guard_number
        guard_number += 1
        inverse = {'bne': 'beq', 'beq': 'bne', 'bcs': 'bcc', 'bcc': 'bcs'}[match[1]]
        label = f'bridge_guard_{guard_number}'
        return f'    {inverse} {label}\n    jmp unsupported\n{label}:\n'
    host = re.sub(r'    (bne|beq|bcs|bcc) unsupported_early\n', far_guard, host)
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
    parser.add_argument('--multisong', action='store_true')
    parser.add_argument('--uploaded-instrument', action='store_true')
    parser.add_argument('--two-instruments', action='store_true')
    parser.add_argument('--multiblock', action='store_true')
    parser.add_argument('--instrument-profiles', action='store_true')
    parser.add_argument('--one-shot', action='store_true')
    parser.add_argument('--brr-profiles', action='store_true')
    parser.add_argument('--relocatable', action='store_true')
    parser.add_argument('--atomic-upload', action='store_true')
    parser.add_argument('--upload-recovery', action='store_true')
    parser.add_argument('--instrument-mapping', action='store_true')
    args = parser.parse_args()
    try:
        image = build(multisong=args.multisong, uploaded_instrument=args.uploaded_instrument,
                      two_instruments=args.two_instruments, multiblock=args.multiblock,
                      instrument_profiles=args.instrument_profiles, one_shot=args.one_shot,
                      brr_profiles=args.brr_profiles, relocatable=args.relocatable, atomic_upload=args.atomic_upload, upload_recovery=args.upload_recovery, instrument_mapping=args.instrument_mapping)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
