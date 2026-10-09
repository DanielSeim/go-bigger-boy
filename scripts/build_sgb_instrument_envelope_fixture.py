#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned resident-instrument held, early-release and retrigger envelope fixtures."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = {'held': (64,127,(24,)), 'short': (64,63,(24,)),
         'retrigger': (16,127,(24,25))}


def bank(instrument=10, case='held'):
    if type(instrument) is not int or instrument not in (2,10) or case not in CASES:
        raise ValueError('requires instrument 2/10 and a known envelope case')
    duration,articulation,notes = CASES[case]
    data = bytearray(64)
    struct.pack_into('<H',data,0,0x2B10)
    struct.pack_into('<HH',data,16,0x2B20,0)
    for channel in (2,3):
        struct.pack_into('<H',data,32+2*channel,0x2B00+len(data))
        data.extend((0xE0,instrument,0xE1,10,0xED,127))
        if channel == 2:
            data.extend((0xE5,160,0xE7,96))
        data.extend((duration,articulation,*(0x80+note for note in notes),1,0xC9,0))
    return bytes(data)


def build(instrument=10, case='held'):
    data = bank(instrument,case)
    payload = struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--instrument',type=int,choices=(2,10),default=10)
    parser.add_argument('--case',choices=CASES,default='held')
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    try:
        image = build(args.instrument,args.case)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(f'Owned envelope fixture: {hashlib.sha256(image).hexdigest()}')
