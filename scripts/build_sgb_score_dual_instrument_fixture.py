#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned two-instrument uploads with voice-local switching and inheritance."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_directory_fixture import bank as directory_bank
from build_sgb_score_instrument_fixture import instrument
from build_sgb_song_selection_fixture import build_cartridge

PROFILES = ('switch', 'inherit', 'repeat', 'maximum', 'end', 'clipped', 'all2', 'all3')
FAULTS = ('source3', 'adsr3', 'start2', 'start3', 'loop3', 'header2', 'header3',
          'padding3', 'missing', 'bad-first-id', 'bad-third-id', 'prefix-overflow')


def assets(*, swap=False, fault=None):
    if type(swap) is not bool or (fault is not None and fault not in FAULTS):
        raise ValueError('invalid asset options')
    data = bytearray(64)
    data[:8] = bytes((2, 0x8F, 0x6F, 0xB8, 3, 0x8F, 0x6F, 0xB8))
    struct.pack_into('<4H', data, 8, 0x5020, 0x5020, 0x5030, 0x5030)
    data[32:41] = instrument('step' if swap else 'square')[16:25]
    data[48:57] = instrument('square' if swap else 'step')[16:25]
    if fault == 'missing':
        return bytes(64)
    if fault in ('source3', 'adsr3', 'start2', 'start3', 'loop3', 'header2', 'header3', 'padding3'):
        offset = dict(source3=4, adsr3=5, start2=8, start3=12, loop3=15,
                      header2=32, header3=48, padding3=63)[fault]
        data[offset] ^= 1
    return bytes(data)


def bank(profile='switch', fault=None):
    if profile not in PROFILES or (fault is not None and fault not in FAULTS):
        raise ValueError('invalid bank options')
    data = bytearray(directory_bank())
    first2, first3, next2, next3 = (2, 3, 3, 2)
    if profile == 'inherit':
        first2, first3, next2, next3 = (3, 2, None, 3)
    elif profile in ('all2', 'all3'):
        first2 = first3 = int(profile[-1])
        next2 = next3 = None
    prefixes2, prefixes3 = [first2], [first3]
    if profile in ('repeat', 'maximum'):
        count2, count3 = (3, 3) if profile == 'repeat' else (26, 27)
        prefixes2 = [2 + (index % 2) for index in range(count2)]
        prefixes3 = [3 - (index % 2) for index in range(count3)]
    if fault == 'bad-first-id':
        prefixes2[0] = 4
    if fault == 'prefix-overflow':
        prefixes2 = [2] * 33
    def prefix(ids):
        return bytes(value for instrument_id in ids for value in (0xE0, instrument_id))
    # Retain the preceding finite native note/control geometry and durations.
    stream2 = (prefix(prefixes2) + bytes.fromhex('e1 0a ed 7f e7 60 e5 a0 10 7f 98') +
               (prefix([next2]) if next2 else b'') + bytes.fromhex('18 7f 99 ed 40 00'))
    if profile == 'clipped':
        stream2 = stream2[:-1] + prefix([2]) + b'\0'
    stream3 = (prefix(prefixes3) + bytes.fromhex('e1 0a ed 7f e7 60 10 7f 98') +
               (prefix([next3]) if next3 else b'') + bytes.fromhex('99 ed 40 00'))
    if profile == 'end':
        stream3 = stream3[:-1] + prefix([3]) + b'\0'
    data[0x2F1:0x2F1 + len(stream2)] = stream2
    data[0x4F5:0x4F5 + len(stream3)] = stream3
    if fault == 'bad-third-id':
        # Unique song-3 channel-2 stream; roots 1 and 2 remain valid.
        data[0xE0:0xEB] = bytes((0xE0, 4)) + data[0xE0:0xE9]
    return bytes(data)


def build(order=(1,), *, profile='switch', swap=False, fault=None, active=False, repeat_upload=False, gate_boundary=False):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1, 2, 3, 128) for song in order):
        raise ValueError('requires 1..4 owned song IDs or stop 128')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    if type(gate_boundary) is not bool or (gate_boundary and not active):
        raise ValueError('gate boundary requires a boolean flag and active spacing')
    score, asset = bank(profile, fault), assets(swap=swap, fault=fault)
    payload = (struct.pack('<HH', len(score), 0x2B00) + score +
               struct.pack('<HH', len(asset), 0x5000) + asset + struct.pack('<HH', 0, 0x0400))
    payload += bytes(4096 - len(payload))
    # SOUND is delivered mid-note after six LCD frames. SOU_TRN has extra
    # host handoff latency, so request it after four to precede the gate end.
    commands = []
    for index, song in enumerate(order):
        if repeat_upload and index:
            commands.append(((6 if gate_boundary else 4) if active else 64, bytes((0x49,))))
        commands.append((64 if not index or repeat_upload or not active else (8 if gate_boundary else 6),
                         bytes((0x41, 0, 0, 0, song))))
    return build_cartridge(payload, (1,), commands=commands)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', choices=PROFILES, default='switch')
    parser.add_argument('--swap', action='store_true')
    args = parser.parse_args()
    try:
        image = build(profile=args.profile, swap=args.swap)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
