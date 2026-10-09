#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned octave with explicit silent GB APU or a constant left-only GB pulse."""
import struct
from build_sgb_host_sample_pitch_fixture import build as octave


def build(voice=2,instrument=2,reversed_map=False,active_gb=False):
    if type(active_gb) is not bool:
        raise ValueError('GB activity must be boolean')
    image=bytearray(octave(voice,instrument,reversed_map))
    if image[0x100:0x103]!=bytes((0xC3,0x50,0x01)):
        raise ValueError('unexpected octave entry point')
    # Unused fixed-bank space; preserve all upload/JOYP code and payload.
    start=0x3F00
    registers=[(0x26,0)]
    if active_gb:
        registers += [(0x26,0x80),(0x24,0x77),(0x25,0x10),(0x10,0),
                      (0x11,0x80),(0x12,0x40),(0x13,0),(0x14,0x87)]
    code=bytes(value for address,value in registers for value in (0x3E,value,0xE0,address))+bytes((0xC3,0x50,0x01))
    if any(image[start:start+len(code)]):
        raise ValueError('GB tone trampoline overlaps fixture')
    image[start:start+len(code)]=code
    image[0x100:0x103]=bytes((0xC3,0,0x3F))
    image[0x14E:0x150]=bytes(2)
    struct.pack_into('>H',image,0x14E,sum(image)&0xFFFF)
    return bytes(image)


if __name__=='__main__':
    import argparse
    import hashlib
    from pathlib import Path
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--instrument',type=int,choices=(2,10),default=2)
    parser.add_argument('--reversed-map',action='store_true')
    parser.add_argument('--active-gb',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.voice,args.instrument,args.reversed_map,args.active_gb)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
