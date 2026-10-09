#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned single-voice octave and reversible sample map for native PCM capture."""
import struct
from build_sgb_owned_sample_fixture import calibrated_assets
from build_sgb_score_tuning_fixture import objects as octave_objects
from build_sgb_score_atomic_fixture import build_cartridge


def objects(voice=2,instrument=2,reversed_map=False):
    if type(voice) is not int or voice not in (2,3) or type(instrument) is not int or instrument not in (2,10) or type(reversed_map) is not bool:
        raise ValueError('requires voice 2/3, instrument 2/10 and boolean map reversal')
    ids=(10,2) if reversed_map else (2,10)
    slot=ids.index(instrument)+2
    score,_=octave_objects(ids,score_profile='octave',tuning=(0,0),octave_slots=(slot,slot))
    score=bytearray(score)
    for pattern,count in ((0,7),(1,6)):
        inactive=5-voice
        start=struct.unpack_from('<H',score,0x60+16*pattern+2*inactive)[0]-0x2B00
        body=score.index(bytes((16,127)),start,start+32)+2
        score[body:body+count]=bytes((0xC9,))*count
    asset=bytearray(calibrated_assets())
    if reversed_map:
        descriptors=(asset[1:4],asset[5:8])
        starts=[struct.unpack_from('<H',asset,8+4*i)[0]-0x5000 for i in (0,1)]
        waves=[asset[start:start+18] for start in starts]
        for i in (0,1):
            asset[1+4*i:4+4*i]=descriptors[1-i]
            asset[starts[i]:starts[i]+18]=waves[1-i]
        asset[24:26]=bytes(ids)
    return bytes(score),bytes(asset)


def build(voice=2,instrument=2,reversed_map=False):
    score,asset=objects(voice,instrument,reversed_map)
    data=struct.pack('<HH',len(score),0x2B00)+score+struct.pack('<HH',len(asset),0x5000)+asset+struct.pack('<HH',0,0x0400)
    if len(data)>4096:
        raise ValueError('pitch fixture exceeds physical upload bound')
    return build_cartridge([data+bytes(4096-len(data))],[(64,bytes((0x41,0,0,0,1)),None)])


if __name__=='__main__':
    import argparse
    import hashlib
    from pathlib import Path
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--voice',type=int,choices=(2,3),default=2)
    parser.add_argument('--instrument',type=int,choices=(2,10),default=2)
    parser.add_argument('--reversed-map',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build(args.voice,args.instrument,args.reversed_map)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
