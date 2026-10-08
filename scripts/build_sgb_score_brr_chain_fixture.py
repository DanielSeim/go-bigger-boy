#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned one-to-four-block BRR sources, loop points and physical upload ROM."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_dual_instrument_fixture import bank, PROFILES as SCORE_PROFILES
from build_sgb_score_instrument_fixture import instrument
from build_sgb_song_selection_fixture import build_cartridge

SAMPLES = {'single': ((1, 1), (0, 0)), 'pair': ((2, 2), (0, 0)),
           'intro': ((3, 4), (1, 2)), 'full': ((4, 4), (0, 0)),
           'tail': ((4, 4), (3, 3)), 'mixed': ((1, 4), (0, 2))}
FAULTS = ('count-zero', 'count-five', 'count-255', 'count3-zero', 'start2', 'start3',
          'loop-unaligned', 'loop-padding', 'loop-other-source', 'loop-high',
          'loop3-unaligned', 'loop3-padding', 'loop3-other-source',
          'early-end', 'missing-end', 'missing-loop', 'filter', 'range',
          'early-end3', 'missing-end3', 'padding2', 'padding3', 'reserved', 'missing')


def assets(sample_profile='intro', fault=None):
    if sample_profile not in SAMPLES or (fault is not None and fault not in FAULTS):
        raise ValueError('unknown sample profile or fault')
    counts, loops = SAMPLES[sample_profile]
    square, step = (instrument(timbre)[17:25] for timbre in ('square', 'step'))
    # Independently authored filter-0 blocks; no original sample is consulted.
    waves = ((square, bytes((0x99,) * 4 + (0x77,) * 4), bytes((0x33,) * 4 + (0xDD,) * 4), step),
             (step, bytes.fromhex('90 ba dc fe 07 65 43 21'), bytes((0x70, 0x90) * 4), square))
    data = bytearray(192)
    data[:8] = bytes((2, 0x8F, 0x6F, 0xB8, 3, 0x8F, 0x6F, 0xB8))
    for voice, base in enumerate((0x40, 0x80)):
        struct.pack_into('<HH', data, 8 + voice * 4, 0x5000 + base, 0x5000 + base + loops[voice] * 9)
        data[16 + voice] = counts[voice]
        for block in range(counts[voice]):
            offset = base + block * 9
            data[offset:offset + 9] = bytes((0xB3 if block + 1 == counts[voice] else 0xB0,)) + waves[voice][block]
    if fault == 'missing':
        return bytes(192)
    if fault:
        # Faults target the default intro layout so their intent is unambiguous.
        if sample_profile != 'intro':
            raise ValueError('fault fixtures require intro profile')
        byte_faults = {'count-zero': (16, 0), 'count-five': (16, 5), 'count-255': (16, 255),
                       'count3-zero': (17, 0), 'start2': (8, 0x49), 'start3': (12, 0x89),
                       'loop-unaligned': (10, 0x4A), 'loop-padding': (10, 0x5B),
                       'loop-other-source': (10, 0x80), 'loop-high': (11, 0x51),
                       'loop3-unaligned': (14, 0x93), 'loop3-padding': (14, 0xA4),
                       'loop3-other-source': (14, 0x40), 'early-end': (0x40, 0xB3),
                       'missing-end': (0x52, 0xB0), 'missing-loop': (0x52, 0xB1),
                       'filter': (0x49, 0xB4), 'range': (0x49, 0xC0),
                       'early-end3': (0x89, 0xB3), 'missing-end3': (0x9B, 0xB0),
                       'padding2': (0x5B, 1), 'padding3': (0xA4, 1), 'reserved': (18, 1)}
        offset, value = byte_faults[fault]
        data[offset] = value
    return bytes(data)


def build(order=(1,), *, sample_profile='intro', score_profile='switch', fault=None,
          active=False, repeat_upload=False):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1, 2, 3, 128) for song in order):
        raise ValueError('requires 1..4 owned song IDs or stop 128')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    score, asset = bank(score_profile), assets(sample_profile, fault)
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
    parser.add_argument('--score-profile', choices=SCORE_PROFILES, default='switch')
    args = parser.parse_args()
    try:
        image = build(sample_profile=args.sample_profile, score_profile=args.score_profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
