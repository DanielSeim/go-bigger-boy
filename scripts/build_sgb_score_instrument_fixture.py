#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned bounded instrument-2 object, uploaded alongside the three-song bank."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_directory_fixture import bank
from build_sgb_song_selection_fixture import build_cartridge

TIMBRES = ('square', 'step')
FAULTS = ('source', 'adsr', 'gain', 'start', 'loop', 'header', 'reserved', 'padding', 'missing')


def instrument(timbre='square', fault=None):
    if timbre not in TIMBRES or (fault is not None and fault not in FAULTS):
        raise ValueError('unknown instrument timbre or fault')
    data = bytearray(32)
    data[:4] = bytes((2, 0x8F, 0x6F, 0xB8))
    struct.pack_into('<HH', data, 8, 0x5010, 0x5010)
    # Independently authored filter-0, range-11, looping single-block waves.
    data[16:25] = bytes((0xB3,)) + (bytes((0x77,) * 4 + (0x99,) * 4)
                                      if timbre == 'square' else bytes((0x12, 0x34, 0x56, 0x70, 0xFE, 0xDC, 0xBA, 0x90)))
    if fault == 'missing':
        return bytes(32)
    if fault:
        offset = dict(source=0, adsr=1, gain=3, start=8, loop=11,
                      header=16, reserved=4, padding=31)[fault]
        data[offset] ^= 1
    return bytes(data)


def build(order=(1,), *, timbre='square', fault=None, active=False, repeat_upload=False):
    if not isinstance(order, (tuple, list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1, 2, 3, 128) for song in order):
        raise ValueError('requires 1..4 song IDs in 1..3 or stop 128')
    if type(active) is not bool or type(repeat_upload) is not bool:
        raise ValueError('spacing flags must be boolean')
    score, asset = bank(), instrument(timbre, fault)
    payload = (struct.pack('<HH', len(score), 0x2B00) + score +
               struct.pack('<HH', len(asset), 0x5000) + asset + struct.pack('<HH', 0, 0x0400))
    payload += bytes(4096 - len(payload))
    commands = []
    for index, song in enumerate(order):
        if repeat_upload and index:
            commands.append((8 if active else 64, bytes((0x49,))))
        commands.append((64 if not index or repeat_upload or not active else 8,
                         bytes((0x41, 0, 0, 0, song))))
    return build_cartridge(payload, (1,), commands=commands)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timbre', choices=TIMBRES, default='square')
    args = parser.parse_args()
    try:
        image = build(timbre=args.timbre)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
