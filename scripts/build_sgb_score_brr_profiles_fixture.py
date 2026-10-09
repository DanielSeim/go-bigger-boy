#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned range-8/11, filter-0..3 sample objects and physical D2 uploads."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_one_shot_fixture import assets as base_assets, SAMPLES
from build_sgb_score_instrument_profiles_fixture import bank, PROFILES, SCORE_PROFILES
from build_sgb_song_selection_fixture import build_cartridge

BRR_PROFILES = tuple(f'f{f}r{r}' for r in (8, 11) for f in range(4)) + ('mixed', 'swap')
FAULTS = {**{'range'+str(r)+'-'+str(source): (base+9, (r << 4) | 12)
            for r in (0, 7, 9, 10, 12, 13, 14, 15) for source, base in ((2, 64), (3, 128))},
          'early-end2': (64, 0x8D), 'early-end3': (128, 0x8F),
          'intermediate-loop': (73, 0xBE), 'missing-end2': (82, 0xBC),
          'missing-end3': (155, 0xBC), 'terminal-loop-only': (155, 0xBE),
          'mode2': (19, 2), 'loop-word3': (14, 0x89), 'padding2': (91, 1),
          'count-five': (17, 5), 'tuning': (22, 2), 'reserved': (20, 1), 'missing': None}


def header_profile(profile, source, block):
    if profile not in BRR_PROFILES:
        raise ValueError('unknown BRR profile')
    if profile.startswith('f'):
        return int(profile[3:]), int(profile[1])
    index = block + (source - 2)
    if profile == 'swap':
        index = 3 - index
    return (8 if index % 2 == 0 else 11), index % 4


def assets(sample_profile='loop', profile='baseline', brr_profile='mixed', fault=None):
    if fault is not None and fault not in FAULTS:
        raise ValueError('unknown fault')
    if fault and sample_profile != 'intro':
        raise ValueError('fault fixtures require intro samples')
    data = bytearray(base_assets(sample_profile, profile))
    # Validate the profile even when the object will be replaced by a missing fixture.
    header_profile(brr_profile, 2, 0)
    for index, base in enumerate((64, 128)):
        for block in range(data[16+index]):
            r, f = header_profile(brr_profile, 2+index, block)
            offset = base+9*block
            data[offset] = (r << 4) | (f << 2) | (data[offset] & 3)
    if fault == 'missing':
        return bytes(192)
    if fault:
        offset, value = FAULTS[fault]
        data[offset] = value
    return bytes(data)


def build(order=(1,), *, sample_profile='loop', profile='baseline', brr_profile='mixed',
          score_profile='switch', fault=None, active=False, repeat_upload=False):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1, 2, 3, 128) for song in order):
        raise ValueError('requires 1..4 owned song IDs or stop 128')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    score, asset = bank(score_profile), assets(sample_profile, profile, brr_profile, fault)
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
    parser.add_argument('--brr-profile', choices=BRR_PROFILES, default='mixed')
    parser.add_argument('--sample-profile', choices=SAMPLES, default='loop')
    parser.add_argument('--profile', choices=PROFILES, default='baseline')
    parser.add_argument('--score-profile', choices=SCORE_PROFILES, default='switch')
    args = parser.parse_args()
    try:
        image = build(brr_profile=args.brr_profile, sample_profile=args.sample_profile,
                      profile=args.profile, score_profile=args.score_profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
