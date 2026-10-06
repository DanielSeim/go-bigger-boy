#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Inventory title command demand against the opt-in original SGB mailbox.

A JOYP trace does not contain SOU_TRN payloads or SPC adoption acknowledgments.
This report never qualifies title playback or proprietary acoustic equivalence.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

from report_sgb_commands import COMMANDS, read_commands

HOST_GAPS = {0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x12, 0x18, 0x19}


def sound_gaps(packet: bytes, version: int) -> list[str]:
    if len(packet) != 16 or packet[0] != 0x41:
        return ['unsupported SOUND framing']
    effect_limit = {1: 1, 2: 1, 3: 3, 4: 5, 5: 5, 6: 5, 7: 5, 8: 5, 9: 5}[version]
    gaps = []
    for label, value in zip(('A', 'B'), packet[1:3]):
        if value not in (0, 0x80) and not 1 <= value <= effect_limit:
            gaps.append(f'unsupported effect {label} 0x{value:02X}')
    flags, score = packet[3:5]
    if version == 1 and flags:
        gaps.append('v1 requires zero attributes')
    elif version != 1 and flags & 0xC0 == 0xC0:
        gaps.append('reserved B volume')
    if score not in ((0,) if version < 3 else (0, 1, 0x80) if version >= 5 else (0, 1, 2, 0x80)):
        gaps.append(f'unsupported score 0x{score:02X}')
    return gaps


def audit(path: Path, version=4) -> dict:
    if type(version) is not int or version not in (1, 2, 3, 4, 5, 6, 7, 8, 9):
        raise ValueError('mailbox version must be 1..9')
    records = read_commands(path)
    if any(a.sequence >= b.sequence for a, b in zip(records, records[1:])):
        raise ValueError('command sequences must increase for ownership analysis')
    counts = Counter(record.command for record in records)
    effects = {'A': Counter(), 'B': Counter(), 'score': Counter()}
    events, unknown_after_transfer = [], False
    first_transfer = None
    gap_counts = Counter()
    for record in records:
        if record.command not in (0x08, 0x09):
            continue
        event = {'sequence': record.sequence, 'command': COMMANDS[record.command][0],
                 'mailbox_evidence': 'unverified_after_transfer_request' if unknown_after_transfer else 'initial_profile_assumed'}
        if record.command == 0x09:
            event['gaps'] = ([] if len(record.packet) == 16 and record.packet[0] == 0x49
                             else ['unsupported SOU_TRN framing'])
            event['payload_evidence'] = 'absent_from_JOYP_trace'
            # Even a malformed request is a boundary: the prototype would halt;
            # the inventory must not imply later commands can execute normally.
            unknown_after_transfer = True
            if first_transfer is None:
                first_transfer = record.sequence
        else:
            event['gaps'] = sound_gaps(record.packet, version)
            event['parameters_in_subset'] = not event['gaps']
            if len(record.packet) == 16:
                for label, value in zip(('A', 'B', 'score'), (record.packet[1], record.packet[2], record.packet[4])):
                    effects[label][value] += 1
        gap_counts.update(event['gaps'])
        events.append(event)
    return {'schema': 'gbb-sgb-original-demand-v1', 'capture': path.name,
            'mailbox_profile': version, 'qualification': False,
            'evidence': 'JOYP_request_inventory',
            'command_counts': {f'0x{key:02X}': value for key, value in sorted(counts.items())},
            'sound_codes': {label: {f'0x{code:02X}': count for code, count in sorted(values.items())}
                            for label, values in effects.items()},
            'parameter_gaps': dict(sorted(gap_counts.items())),
            'first_transfer_sequence': first_transfer,
            'ignored_host_commands': {COMMANDS[key][0]: counts[key] for key in sorted(HOST_GAPS & counts.keys())},
            'audio_events': events}


def describe(result: dict) -> str:
    lines = [f"{result['capture']}: mailbox v{result['mailbox_profile']} request inventory; title qualification=false"]
    for label, counts in result['sound_codes'].items():
        lines.append(f"  {label}: " + (', '.join(f'{code} x{count}' for code, count in counts.items()) or 'no requests'))
    for reason, count in result['parameter_gaps'].items():
        lines.append(f'  parameter gap: {reason} x{count}')
    if result['first_transfer_sequence'] is not None:
        lines.append(f"  SOU_TRN boundary #{result['first_transfer_sequence']}: payload and later mailbox ownership require separate evidence")
    for name, count in result['ignored_host_commands'].items():
        lines.append(f'  original host does not execute: {name} x{count}')
    lines.append('  Parameter coverage does not establish sound assets, timing, transfer validity or title playback.')
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', nargs='+', type=Path)
    parser.add_argument('--mailbox-version', type=int, choices=(1, 2, 3, 4, 5, 6, 7, 8, 9), default=4)
    parser.add_argument('--json', action='store_true', help='Emit bounded metadata, never raw JOYP packets')
    args = parser.parse_args()
    try:
        results = [audit(path, args.mailbox_version) for path in args.traces]
        print(json.dumps(results, indent=2, sort_keys=True) if args.json else '\n'.join(map(describe, results)))
    except (OSError, UnicodeError, ValueError) as error:
        parser.exit(2, f'error: {error}\n')


if __name__ == '__main__':
    main()
