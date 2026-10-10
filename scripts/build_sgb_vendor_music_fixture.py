#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored single-channel music banks and separate data-only asset transfers."""
import argparse
import hashlib
import struct
from pathlib import Path
from build_sgb_score_atomic_fixture import build_cartridge


def bank(*, fault=None, echo=True):
    data = bytearray(256)
    struct.pack_into('<3H', data, 0, 0x2B10, 0x2B14, 0x2B18)
    for index in range(3):
        table = 0x2B20 + 16*index
        track = 0x2B60 + 48*index
        struct.pack_into('<HH', data, 16+4*index, table, 0)
        struct.pack_into('<8H', data, table-0x2B00, 0, 0, track, 0, 0, 0, 0, 0)
        events = bytes((0xE0, 2 if index != 1 else 10, 0xE1, 10,
                        0xE5, 160, 0xED, 127, 0xE7, 96,
                        0xF7, 1, 32, 0, 0xF5, 4 if echo else 0, 16, 24,
                        16, 127, 0x98+index, 0x99+index, 0xA4+index,
                        1, 0xC9, 0))
        data[track-0x2B00:track-0x2B00+len(events)] = events
    if fault == 'late-opcode':
        data[0x60+22] = 0xFF
    elif fault == 'root-outside':
        struct.pack_into('<H', data, 0, 0x2C00)
    elif fault == 'root-before':
        struct.pack_into('<H', data, 0, 0x2AFF)
    elif fault == 'channel':
        struct.pack_into('<H', data, 0x20+6, 0x2B60)
    elif fault == 'zero-tempo':
        data[0x69] = 0
    elif fault == 'instrument':
        data[0x61] = 9
    elif fault == 'filter':
        data[0x6D] = 4
    elif fault == 'pan':
        data[0x63] = 21
    elif fault == 'truncated':
        data = data[:0x6C]
    elif fault is not None:
        raise ValueError('unknown score fault')
    return bytes(data)


def payload(chunks, entry=0x0400):
    result = b''.join(struct.pack('<HH', len(data), address)+data for address, data in chunks)
    result += struct.pack('<HH', 0, entry)
    if len(result) > 4096:
        raise ValueError('physical transfer bound')
    return result+bytes(4096-len(result))


def assets(*, descriptor=(2,0x8E,0xAF,0,0x10,0), sample=bytes((0xA3,))+bytes((0x22,))*8,
           start=0x3B00, loop=0x3B00):
    return ((0x4B08, struct.pack('<HH',start,loop)), (0x4C3C,bytes(descriptor)), (0x3B00,sample))


def build(case='complete', fault=None):
    score = bank(fault=fault, echo=case != 'dry')
    score_frame = payload(((0x2B00, score),))
    # Authored instrument-2 binding and looping BRR, different from residents.
    asset_frame = payload(assets())
    sound = lambda song, attr=0: bytes((0x41,0,0,attr,song))
    if case in ('complete', 'dry'):
        return build_cartridge((score_frame,), ((16,sound(1),None),))
    if case == 'split-assets':
        return build_cartridge((score_frame,asset_frame), ((4,bytes((0x49,)),1),(16,sound(1),None)))
    if case == 'switch':
        return build_cartridge((score_frame,), ((16,sound(1),None),(4,sound(2),None),(4,sound(3),None)))
    if case == 'stop':
        return build_cartridge((score_frame,), ((16,sound(1),None),(4,sound(128),None)))
    if case == 'mute':
        return build_cartridge((score_frame,), ((16,sound(1,0x8C),None),))
    if case == 'unmute':
        return build_cartridge((score_frame,), ((16,sound(1,0x8C),None),(4,sound(0),None)))
    if case == 'upload-active':
        return build_cartridge((score_frame,asset_frame), ((16,sound(1),None),(4,bytes((0x49,)),1),(16,sound(2),None)))
    if case == 'code-write':
        return build_cartridge((payload(((0x0800,bytes(8)),)),), ((16,sound(1),None),))
    if case == 'asset-only-cold':
        return build_cartridge((asset_frame,), ((16,sound(1),None),))
    if case in ('asset-wrap', 'asset-overflow'):
        address = 0xFFF8 if case == 'asset-wrap' else 0x4CF8
        return build_cartridge((payload(((address,bytes(8 if case == 'asset-wrap' else 9)),)),), ((16,sound(1),None),))
    if case == 'bad-entry':
        return build_cartridge((payload(((0x2B00,score),),0x0800),), ((16,sound(1),None),))
    raise ValueError('unknown fixture case')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', default='complete')
    parser.add_argument('--fault')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        image = build(args.case, args.fault)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
