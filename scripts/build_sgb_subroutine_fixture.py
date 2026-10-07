#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build owned songs isolating finite subroutine repetition and return."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = {'once': 1, 'twice': 2, 'thrice': 3}


def score_payload(case='once'):
    if case not in CASES:
        raise ValueError('unknown subroutine fixture case')
    bank = bytearray(64)
    struct.pack_into('<H', bank, 0, 0x2B10)
    struct.pack_into('<HH', bank, 16, 0x2B14, 0)
    struct.pack_into('<H', bank, 24, 0x2B24)  # Pattern channel 2 only.
    # Caller sets controls, calls $2B40, then plays a distinguishable continuation
    # without setting duration/articulation: those come from the callee.
    bank[36:53] = bytes((0xE0,2,0xE1,10,0xE5,160,0xED,127,0xE7,96,
                         0xEF,0x40,0x2B,CASES[case],0xA4,0x98,0))
    bank.extend((16,0x7F,0x98,0x99,0))
    data = struct.pack('<HH',len(bank),0x2B00) + bank + struct.pack('<HH',0,0x0400)
    return data + bytes(4096-len(data))


def build(case='once'):
    return build_cartridge(score_payload(case),(1,))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=tuple(CASES),default='once')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(f'Original subroutine fixture: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__':
    main()
