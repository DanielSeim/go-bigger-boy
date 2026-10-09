#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned relocatable samples inside two fixed 64-byte D3 windows."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_brr_profiles_fixture import assets as base_assets, BRR_PROFILES
from build_sgb_score_one_shot_fixture import SAMPLES
from build_sgb_score_instrument_profiles_fixture import bank, PROFILES, SCORE_PROFILES
from build_sgb_song_selection_fixture import build_cartridge

LAYOUTS = ('base', 'near', 'last', 'source2', 'source3')
# Faults use the three/four-block intro object and near layout.
FAULTS = {}
for source, base, entry, count in ((2, 64, 8, 3), (3, 128, 12, 4)):
    for label, value in (('metadata', 0x5010), ('code', 0x1800), ('score', 0x2B00),
                         ('cache', 0x6000), ('before', 0x5000+base-1),
                         ('end', 0x5000+base+64), ('unaligned', 0x5000+base+1),
                         ('cross', 0x5000+(128 if source == 2 else 64)),
                         ('overflow', 0x5000+base+9*((64-9*count)//9+1))):
        FAULTS[f'start-{label}{source}'] = (entry, value, 2)
    start = base + (9 if source == 2 else 18)
    for label, value in (('prefix', 0x5000+base), ('unaligned', 0x5000+start+1),
                         ('past-chain', 0x5000+start+9*count),
                         ('high', 0x5100+start), ('cross', 0x5000+(128 if source == 2 else 64))):
        FAULTS[f'loop-{label}{source}'] = (entry+2, value, 2)
    FAULTS[f'prefix-byte{source}'] = (base, 1, 1)
    FAULTS[f'suffix-byte{source}'] = (base+63, 1, 1)
    FAULTS[f'count-five{source}'] = (16+source-2, 5, 1)
    FAULTS[f'mode{source}'] = (19+(source-2)*4, 2, 1)
    FAULTS[f'early-end{source}'] = (start, 0x8D, 1)
    FAULTS[f'oneshot-loop{source}'] = (entry+2, 0x5000+start+9, 2)


def assets(sample_profile='loop', profile='baseline', brr_profile='mixed', layout='near', fault=None):
    if layout not in LAYOUTS or (fault is not None and fault not in FAULTS):
        raise ValueError('unknown layout or fault')
    if fault and (sample_profile != 'intro' or layout != 'near'):
        raise ValueError('fault fixtures require intro samples and near layout')
    data = bytearray(base_assets(sample_profile, profile, brr_profile))
    for source, base in enumerate((64, 128)):
        count = data[16+source]
        blocks = (0 if layout == 'base' or layout == f'source{3-source}' else
                  (64-9*count)//9 if layout == 'last' else 1+source)
        start = base+9*blocks
        chain = data[base:base+9*count]
        old_loop = int.from_bytes(data[10+4*source:12+4*source], 'little')
        data[base:base+64] = bytes(64)
        data[start:start+9*count] = chain
        data[8+4*source:10+4*source] = (0x5000+start).to_bytes(2, 'little')
        data[10+4*source:12+4*source] = (old_loop+9*blocks).to_bytes(2, 'little')
    if fault:
        offset, value, size = FAULTS[fault]
        data[offset:offset+size] = value.to_bytes(size, 'little')
    return bytes(data)


def build(order=(1,), *, sample_profile='loop', profile='baseline', brr_profile='mixed',
          score_profile='switch', layout='near', fault=None, active=False, repeat_upload=False):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1, 2, 3, 128) for song in order):
        raise ValueError('requires 1..4 owned song IDs or stop 128')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    score = bank(score_profile)
    asset = assets(sample_profile, profile, brr_profile, layout, fault)
    payload = (struct.pack('<HH', len(score), 0x2B00) + score +
               struct.pack('<HH', len(asset), 0x5000) + asset + struct.pack('<HH', 0, 0x0400))
    payload += bytes(4096-len(payload))
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
    parser.add_argument('--layout', choices=LAYOUTS, default='near')
    parser.add_argument('--brr-profile', choices=BRR_PROFILES, default='mixed')
    parser.add_argument('--sample-profile', choices=SAMPLES, default='loop')
    parser.add_argument('--profile', choices=PROFILES, default='baseline')
    parser.add_argument('--score-profile', choices=SCORE_PROFILES, default='switch')
    args = parser.parse_args()
    try:
        image = build(layout=args.layout, brr_profile=args.brr_profile, sample_profile=args.sample_profile,
                      profile=args.profile, score_profile=args.score_profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
