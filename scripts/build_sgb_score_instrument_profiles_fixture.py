#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned uploaded ADSR and half-pitch profiles for the bounded D0 diagnostic."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_brr_chain_fixture import assets as sample_assets
from build_sgb_score_dual_instrument_fixture import bank as instrument_bank, PROFILES as BASE_SCORE_PROFILES
from build_sgb_song_selection_fixture import build_cartridge

SCORE_PROFILES = (*BASE_SCORE_PROFILES, 'default')

# (ADSR1, ADSR2, GAIN, half-pitch flag) independently chosen diagnostic settings.
PROFILES = {'baseline': ((0x8F, 0x6F, 0xB8, 0), (0x8F, 0x6F, 0xB8, 0)),
            'envelope': ((0xFF, 0x4C, 0xB8, 0), (0x8F, 0x6F, 0xB8, 0)),
            'tuning': ((0x8F, 0x6F, 0xB8, 1), (0x8F, 0x6F, 0xB8, 0)),
            'distinct': ((0xFF, 0x4C, 0xB8, 1), (0x8F, 0x6F, 0xB8, 0)),
            'swapped': ((0x8F, 0x6F, 0xB8, 0), (0xFF, 0x4C, 0xB8, 1)),
            'gain': ((0, 0, 0x40, 1), (0, 0, 0x60, 0)),
            'decay': ((0xFF, 0x6F, 0xB8, 0), (0x8A, 0x4C, 0xB8, 1)),
            'cross': ((0x8F, 0x4C, 0xB8, 0), (0x8A, 0x6F, 0xB8, 1))}
FAULTS = {'source2': (0, 3), 'source3': (4, 2), 'adsr-off2': (1, 0),
          'adsr-off3': (5, 0), 'attack2': (1, 0x8B), 'attack3': (5, 0x8B),
          'sustain2': (2, 0x4D), 'sustain3': (6, 0x4D),
          'gain2': (3, 0xB9), 'gain3': (7, 0xB9),
          'tuning2': (18, 2), 'tuning3': (22, 2),
          'tuning2-255': (18, 255), 'tuning3-255': (22, 255),
          'reserved': (19, 1), 'gain-adsr2': None, 'gain-range2': None,
          'gain-adsr3': None, 'gain-range3': None, 'missing': None,
          **{'brr-' + name: None for name in
             ('count-zero', 'count-five', 'loop-other-source', 'early-end', 'filter', 'padding3')}}


def bank(profile='switch'):
    if profile != 'default':
        return instrument_bank(profile)
    data = bytearray(instrument_bank())
    # Remove just the first E0 in each shared first-pattern stream. Both voices
    # must use uploaded instrument 2 until a later executed selection.
    for start in (0x2F1, 0x4F5):
        end = data.index(0, start)
        data[start:end+1] = data[start+2:end+1] + bytes(2)
    return bytes(data)


def assets(profile='distinct', fault=None):
    if profile not in PROFILES or (fault is not None and fault not in FAULTS):
        raise ValueError('unknown instrument profile or fault')
    data = bytearray(sample_assets(fault=fault[4:] if fault and fault.startswith('brr-') else None))
    for instrument, (attack, sustain, gain, tuning) in enumerate(PROFILES[profile]):
        data[1 + 4 * instrument:4 + 4 * instrument] = bytes((attack, sustain, gain))
        data[18 + 4 * instrument] = tuning
    if fault == 'missing':
        return bytes(192)
    if fault and fault.startswith('gain-'):
        index = int(fault[-1]) - 2
        data[1+4*index:4+4*index] = bytes((0, 1, 0x40) if 'adsr' in fault else (0, 0, 0x41))
    elif fault and not fault.startswith('brr-'):
        offset, value = FAULTS[fault]
        data[offset] = value
    return bytes(data)


def build(order=(1,), *, profile='distinct', score_profile='switch', fault=None,
          active=False, repeat_upload=False):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1, 2, 3, 128) for song in order):
        raise ValueError('requires 1..4 owned song IDs or stop 128')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    score, asset = bank(score_profile), assets(profile, fault)
    payload = (struct.pack('<HH', len(score), 0x2B00) + score +
               struct.pack('<HH', len(asset), 0x5000) + asset + struct.pack('<HH', 0, 0x0400))
    payload += bytes(4096 - len(payload))
    commands = []
    for index, song in enumerate(order):
        if repeat_upload and index:
            commands.append((4 if active else 64, bytes((0x49,))))
        commands.append((64 if not index or repeat_upload or not active else 6,
                         bytes((0x41, 0, 0, 0, song))))
    return build_cartridge(payload, (1,), commands=commands)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', choices=PROFILES, default='distinct')
    parser.add_argument('--score-profile', choices=SCORE_PROFILES, default='switch')
    args = parser.parse_args()
    try:
        image = build(profile=args.profile, score_profile=args.score_profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
