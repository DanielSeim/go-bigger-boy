#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned looping/one-shot BRR objects and physical uploads for D1."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_brr_chain_fixture import assets as sample_assets
from build_sgb_score_instrument_profiles_fixture import bank, PROFILES, SCORE_PROFILES
from build_sgb_song_selection_fixture import build_cartridge

# Counts, per-source one-shot flags, loop block indices. One-shots are canonical
# to start; the DSP may still redirect inaudible reads after the terminal block.
SAMPLES = {'loop': ((3, 4), (0, 0), (1, 2)),
           'one': ((1, 1), (1, 1), (0, 0)),
           'pair': ((2, 2), (1, 1), (0, 0)),
           'triple': ((3, 3), (1, 1), (0, 0)),
           'intro': ((3, 4), (1, 1), (0, 0)),
           'full': ((4, 4), (1, 1), (0, 0)),
           'mixed2': ((3, 4), (1, 0), (0, 2)),
           'mixed3': ((3, 4), (0, 1), (1, 0))}
FAULTS = {'mode2': (19, 2), 'mode3': (23, 2), 'mode2-255': (19, 255), 'mode3-255': (23, 255),
          'one-loop2': (0x52, 0xB3), 'one-loop3': (0x9B, 0xB3),
          'loop-one2': (19, 0), 'loop-one3': (23, 0),
          'early-end2': (0x40, 0xB1), 'early-end3': (0x80, 0xB1),
          'missing-end2': (0x52, 0xB0), 'missing-end3': (0x9B, 0xB0),
          'loop-word2': (10, 0x49), 'loop-word3': (14, 0x89),
          'loop-high': (11, 0x51), 'loop-other-source': (10, 0x80),
          'padding2': (0x5B, 1), 'padding3': (0xA4, 1),
          'count-zero': (16, 0), 'count-five': (17, 5),
          'filter': (0x49, 0xB4), 'range': (0x89, 0xC0),
          'adsr': (5, 0x8B), 'tuning': (22, 2), 'reserved': (20, 1), 'missing': None}


def assets(sample_profile='intro', profile='baseline', fault=None):
    if sample_profile not in SAMPLES or profile not in PROFILES or (fault is not None and fault not in FAULTS):
        raise ValueError('unknown sample/instrument profile or fault')
    if fault and sample_profile != 'intro':
        raise ValueError('fault fixtures require intro samples')
    data = bytearray(sample_assets('full'))
    counts, modes, loops = SAMPLES[sample_profile]
    for index, base in enumerate((0x40, 0x80)):
        attack, sustain, gain, tuning = PROFILES[profile][index]
        data[4*index:4*index+4] = bytes((2+index, attack, sustain, gain))
        data[16+index] = counts[index]
        data[18+4*index:20+4*index] = bytes((tuning, modes[index]))
        struct.pack_into('<H', data, 10+4*index, 0x5000+base+9*loops[index])
        for block in range(counts[index]):
            data[base+9*block] = (0xB1 if modes[index] else 0xB3) if block+1 == counts[index] else 0xB0
        data[base+9*counts[index]:base+64] = bytes(64-9*counts[index])
    if fault == 'missing':
        return bytes(192)
    if fault:
        offset, value = FAULTS[fault]
        data[offset] = value
    return bytes(data)


def build(order=(1,), *, sample_profile='intro', profile='baseline', score_profile='switch', fault=None,
          active=False, repeat_upload=False):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1, 2, 3, 128) for song in order):
        raise ValueError('requires 1..4 owned song IDs or stop 128')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    score, asset = bank(score_profile), assets(sample_profile, profile, fault)
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
    parser.add_argument('--sample-profile', choices=SAMPLES, default='intro')
    parser.add_argument('--profile', choices=PROFILES, default='baseline')
    parser.add_argument('--score-profile', choices=SCORE_PROFILES, default='switch')
    args = parser.parse_args()
    try:
        image = build(sample_profile=args.sample_profile, profile=args.profile, score_profile=args.score_profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
