#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inventory explicit score roots using a limited common N-SPC width profile.

This is a structural demand scan, not a scheduler, vendor parser qualification,
or sample exporter. Subroutine and phrase-control boundaries stop their paths.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path

BASE = 0x2B00
MAX_READS = 32768
MAX_EVENTS = 8192
MAX_PATTERNS = 64
# Only unambiguous widths needed by current demand analysis. No acoustic
# parameter values or vendor implementation bytes are copied into reports.
# Instrument IDs and subroutine target addresses remain structural metadata.
# E6 is deliberately
# absent: generic descriptions disagree on its width, so variant evidence is
# required before it can be scanned.
CONTROLS = {
    0xE0: ('instrument', 1), 0xE1: ('pan', 1),
    0xE3: ('vibrato', 3), 0xE5: ('song_volume', 1),
    0xE7: ('tempo', 1), 0xE8: ('tempo_fade', 2),
    0xEA: ('transpose', 1), 0xED: ('volume', 1),
    0xEF: ('subroutine', 3), 0xF4: ('fine_tune', 1),
    0xF5: ('echo_enable', 3), 0xF7: ('echo_setup', 3),
}


def inventory(bank, phrase):
    if not isinstance(bank, bytes) or not 1 <= len(bank) <= 8192:
        raise ValueError('bank must contain 1..8192 bytes based at $2B00')
    if type(phrase) is not int:
        raise ValueError('phrase must be an integer SPC address')
    reads = events = patterns = tracks = 0
    counts = Counter()
    channels, instruments, notes = set(), set(), set()
    frontiers = []

    def read(address, size=1):
        nonlocal reads
        reads += 1
        if reads > MAX_READS:
            raise ValueError('inventory read budget exceeded')
        if not BASE <= address <= BASE + len(bank) - size:
            raise ValueError(f'out-of-bank read at ${address:04X} ({size} bytes)')
        return bank[address - BASE:address - BASE + size]

    def word(address):
        return int.from_bytes(read(address, 2), 'little')

    cursor = phrase
    while True:
        pattern = word(cursor)
        cursor += 2
        if pattern == 0:
            break
        if pattern < 0x100:
            frontiers.append({'kind': 'phrase_control', 'address': cursor - 2})
            break
        if patterns >= MAX_PATTERNS:
            raise ValueError('inventory pattern budget exceeded')
        pointers = read(pattern, 16)
        patterns += 1
        for channel in range(8):
            pointer = int.from_bytes(pointers[2*channel:2*channel+2], 'little')
            if pointer == 0:
                continue
            tracks += 1
            channels.add(channel)
            while True:
                address = pointer
                opcode = read(pointer)[0]
                pointer += 1
                if opcode == 0:
                    break
                events += 1
                if events > MAX_EVENTS:
                    raise ValueError('inventory event budget exceeded')
                if opcode < 0x80:
                    counts['duration'] += 1
                    if read(pointer)[0] < 0x80:
                        counts['articulation'] += 1
                        pointer += 1
                    if read(pointer)[0] < 0x80:
                        raise ValueError(f'expected note or control after duration at ${pointer:04X}')
                elif opcode <= 0xC7:
                    counts['note'] += 1
                    notes.add(opcode - 0x80)
                elif opcode <= 0xDF:
                    counts[{0xC8: 'tie', 0xC9: 'rest'}.get(opcode, 'percussion')] += 1
                elif opcode in CONTROLS:
                    kind, width = CONTROLS[opcode]
                    operand = read(pointer, width)
                    pointer += width
                    counts[kind] += 1
                    if kind == 'instrument':
                        instruments.add(operand[0])
                    if kind == 'subroutine':
                        # Do not infer execution/repetition or inherited state
                        # after a call. Its body and continuation are unscanned.
                        frontiers.append({'kind': 'subroutine', 'address': address,
                                          'channel': channel,
                                          'target': int.from_bytes(operand[:2], 'little')})
                        break
                else:
                    frontiers.append({'kind': 'unknown_opcode', 'address': address,
                                      'channel': channel, 'opcode': f'0x{opcode:02X}'})
                    break

    requirements = {'vendor_envelope_and_song_selection', 'vendor_phrase_termination'}
    if channels:
        requirements.add('vendor_channel_allocation')
    if len(channels) > 2:
        requirements.add('more_than_two_music_tracks')
    if notes:
        requirements.add('vendor_pitch_and_tuning')
    if notes and max(notes) > 31:
        requirements.add('wider_base_note_range')
    if counts['instrument'] or counts['percussion']:
        requirements.add('vendor_instrument_and_sample_mapping')
    if counts['articulation']:
        requirements.add('quantization_and_velocity_tables')
    for kind, requirement in (
        ('tempo', 'tempo_scheduler'), ('tempo_fade', 'tempo_fade'),
        ('song_volume', 'vendor_volume_curve'), ('volume', 'vendor_volume_curve'),
        ('pan', 'vendor_pan_curve_and_phase'), ('fine_tune', 'fine_tune'),
        ('transpose', 'vendor_transpose_range_and_pitch'),
        ('vibrato', 'vibrato'), ('echo_enable', 'echo'), ('echo_setup', 'echo'),
        ('subroutine', 'subroutine_execution'), ('percussion', 'percussion')):
        if counts[kind]:
            requirements.add(requirement)
    if any(f['kind'] == 'phrase_control' for f in frontiers):
        requirements.add('phrase_control_execution')
    if any(f['kind'] == 'unknown_opcode' for f in frontiers):
        requirements.add('unrecognized_variant_commands')
    return {'schema': 'gbb-sgb-score-demand-v1', 'profile': 'limited_common_nspc_widths',
            'qualification': False, 'playback': False, 'evidence': 'structural_scan',
            'bank_sha256': hashlib.sha256(bank).hexdigest(), 'bank_bytes': len(bank),
            'phrase': phrase, 'patterns_scanned': patterns, 'tracks_scanned': tracks,
            'channels_observed': sorted(channels), 'instrument_ids_observed': sorted(instruments),
            'base_note_range': [min(notes), max(notes)] if notes else None,
            'structural_counts': dict(sorted(counts.items())),
            'linear_scan_complete': not frontiers, 'frontiers': frontiers,
            'required_contracts': sorted(requirements)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bank', type=Path, help='Raw caller-owned bank beginning at SPC $2B00')
    parser.add_argument('--phrase', required=True, type=lambda value: int(value, 0),
                        help='Explicit phrase-list address; no root discovery')
    args = parser.parse_args()
    try:
        with args.bank.open('rb') as source:
            result = inventory(source.read(8193), args.phrase)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
