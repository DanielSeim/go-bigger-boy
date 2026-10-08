#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned bank upload, $0400 restart and bounded score-1 selection fixture."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_short_pair_fixture import bank
from build_sgb_song_selection_fixture import build_cartridge


def build(order=(1,), *, repeat_upload=False, sound_fields=(0, 0, 0), malformed=False):
    if type(malformed) is not bool:
        raise ValueError('malformed flag must be boolean')
    score = bytearray(bank())
    if malformed:
        score[:2] = b'\xff\xff'  # Root outside the owned 2048-byte bank.
    payload = struct.pack('<HH', len(score), 0x2B00) + score + struct.pack('<HH', 0, 0x0400)
    payload += bytes(4096 - len(payload))
    return build_cartridge(payload, order, wait_frames=64,
                           repeat_upload=repeat_upload, sound_fields=sound_fields)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--repeat-upload', action='store_true')
    parser.add_argument('--order', default='1')
    args = parser.parse_args()
    try:
        image = build(tuple(int(value) for value in args.order.split(',')),
                      repeat_upload=args.repeat_upload)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())


if __name__ == '__main__':
    main()
