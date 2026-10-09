#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned held instrument-10 notes with independently controlled peer streams."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_song_selection_fixture import build_cartridge

CASES = ('retrigger','rests','switch','clipped')


def peer_notes(case):
    if type(case) is not str or case not in CASES:
        raise ValueError('unknown asynchronous envelope case')
    return {'clipped':((10,24),), 'retrigger':((10,24),(10,25),(10,24),(10,25)),
            'rests':((10,24),(10,25)), 'switch':((10,24),(2,25),(10,24),(2,25))}[case]


def bank(case='retrigger', held_voice=2):
    notes = peer_notes(case)
    if type(held_voice) is not int or held_voice not in (2,3):
        raise ValueError('held voice must be 2 or 3')
    data = bytearray(64)
    struct.pack_into('<H',data,0,0x2B10)
    struct.pack_into('<HH',data,16,0x2B20,0)
    for voice in (2,3):
        struct.pack_into('<H',data,32+2*voice,0x2B00+len(data))
        data.extend((0xE0,10,0xE1,10,0xED,127))
        if voice == 2:
            data.extend((0xE5,160,0xE7,96))
        if voice == held_voice:
            data.extend((64,127,0x98,1,0xC9,0))
        else:
            data.extend((16,127))
            for index,(instrument,note) in enumerate(notes):
                if case == 'switch' and index:
                    data.extend((0xE0,instrument))
                data.append(0x80+note)
                if case == 'rests':
                    data.append(0xC9)
            data.extend((0xC9,0) if case == 'clipped' else (1,0xC9,0))
    return bytes(data)


def build(case='retrigger',held_voice=2):
    data = bank(case,held_voice)
    payload = struct.pack('<HH',len(data),0x2B00)+data+struct.pack('<HH',0,0x0400)
    return build_cartridge(payload+bytes(4096-len(payload)),(1,))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',choices=CASES,default='retrigger')
    parser.add_argument('--held-voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    try:
        image = build(args.case,args.held_voice)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(f'Owned asynchronous envelope fixture: {hashlib.sha256(image).hexdigest()}')
