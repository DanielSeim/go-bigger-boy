#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned score-ID/sample-slot maps and physical upload lifecycle fixtures."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_atomic_fixture import build_cartridge
from build_sgb_score_relocated_fixture import assets
from build_sgb_score_instrument_profiles_fixture import bank as base_bank, SCORE_PROFILES, PROFILES

CHUNKS = ('full','split','reverse','interleave','tail')
FAULTS = ('duplicate','missing-map','unknown-first','unknown-third','reserved',
          'source2','source3','mapping-only')


def checked_ids(ids):
    if not isinstance(ids,(tuple,list)) or len(ids) != 2 or any(
            type(value) is not int or not 0 <= value <= 255 for value in ids) or ids[0] == ids[1]:
        raise ValueError('requires two distinct byte instrument IDs')
    return tuple(ids)


def objects(ids=(2,10), *, score_profile='switch', profile='distinct', fault=None):
    ids = checked_ids(ids)
    if score_profile not in SCORE_PROFILES or profile not in PROFILES or fault not in (None,*FAULTS):
        raise ValueError('unknown score, descriptor profile or mapping fault')
    score = bytearray(base_bank(score_profile))
    # These are the two independently owned shared first-pattern streams.
    # Walk the original bytes, skipping each E0 operand before changing it;
    # mapped IDs may themselves be opcode bytes or zero.
    for start in (0x2F1,0x4F5):
        end = score.index(0,start)
        cursor = start
        while cursor < end:
            if score[cursor] == 0xE0:
                old = score[cursor+1]
                if old not in (2,3):
                    raise ValueError('owned stream instrument geometry changed')
                score[cursor+1] = ids[old-2]
                cursor += 2
            else:
                cursor += 1
    data = bytearray(assets('mixed3',profile=profile,brr_profile='mixed',layout='near'))
    data[24:26] = bytes(ids)
    unknown = next(value for value in (3,0,255,1) if value not in ids)
    if fault == 'duplicate':
        data[25] = data[24]
    elif fault == 'missing-map':
        data[24:26] = bytes(2)
    elif fault == 'unknown-first':
        if score_profile == 'default':
            raise ValueError('unknown-first requires an initial E0')
        score[0x2F2] = unknown
    elif fault == 'unknown-third':
        score[0xE0:0xEB] = bytes((0xE0,unknown)) + score[0xE0:0xE9]
    elif fault == 'reserved':
        data[26] = 1
    elif fault in ('source2','source3'):
        data[0 if fault == 'source2' else 4] = 10
    return bytes(score),bytes(data)


def payload(ids=(2,10), *, chunking='full', **options):
    if chunking not in CHUNKS:
        raise ValueError('unknown chunk list')
    score,data = objects(ids,**options)
    chunks = [(0x2B00,score),(0x5000,data)]
    if chunking == 'split':
        chunks = [(0x2B00,score[:1]),(0x2B01,score[1:257]),(0x2C01,score[257:]),
                  (0x5000,data[:25]),(0x5019,data[25:])]
    elif chunking == 'reverse':
        chunks.reverse()
    elif chunking == 'interleave':
        chunks = [(0x2B00,score[:256]),(0x5000,data[:25]),
                  (0x2C00,score[256:]),(0x5019,data[25:])]
    elif chunking == 'tail':
        chunks = [chunks[0],(0x5000,data[:-1]),(0x50BF,data[-1:])]
    if options.get('fault') == 'mapping-only':
        chunks = [(0x5018,data[24:26])]
    result = b''.join(struct.pack('<HH',len(part),address)+part for address,part in chunks)
    result += struct.pack('<HH',0,0x0400)
    if len(result) > 4096:
        raise ValueError('mapping payload exceeds physical frame')
    return result + bytes(4096-len(result))


def build(order=(1,), *, ids=(2,10), score_profile='switch', profile='distinct',
          fault=None, chunking='full', active=False, repeat_upload=False,
          replace=False, replacement_ids=(10,2), recover=False, active_failure=False,
          repeat_failure=False, stop=False, replacement_score_profile=None):
    ids,replacement_ids = checked_ids(ids),checked_ids(replacement_ids)
    if not isinstance(order,(tuple,list)) or not 1 <= len(order) <= 4 or any(
            type(song) is not int or song not in (1,2,3,128) for song in order):
        raise ValueError('requires 1..4 song IDs or stop 128')
    if any(type(flag) is not bool for flag in
           (active,repeat_upload,replace,recover,active_failure,repeat_failure,stop)):
        raise ValueError('lifecycle flags must be boolean')
    if fault is not None:
        if tuple(order) != (1,) or replace or repeat_upload or (repeat_failure and not recover):
            raise ValueError('failure fixtures use one selection and optional recovery')
    elif recover or active_failure or repeat_failure or (replace and (len(order) != 2 or repeat_upload)) or (stop and not replace):
        raise ValueError('invalid lifecycle combination')
    if replacement_score_profile is not None and (replacement_score_profile not in SCORE_PROFILES or
                                                  not (recover or replace)):
        raise ValueError('replacement score requires replacement or recovery')
    def sound(song):
        return bytes((0x41,0,0,0,song))
    options = dict(score_profile=score_profile,profile=profile,chunking=chunking)
    final_options = {**options,'score_profile':replacement_score_profile or score_profile}
    initial = payload(ids,**options)
    commands = []
    if fault is not None:
        bad = payload(ids,fault=fault,**options)
        payloads = [initial,bad] if active_failure else [bad]
        bad_index = int(active_failure)
        if active_failure:
            commands += [(64,sound(1),None),(4,bytes((0x49,)),bad_index)]
        commands += [(16 if active_failure else 64,sound(3 if active_failure else 1),None)]
        if stop:
            commands += [(4,sound(128),None)]
        if repeat_failure:
            commands += [(4,bytes((0x49,)),bad_index),(16,sound(1),None)]
        if recover:
            payloads += [payload(replacement_ids,**final_options)]
            commands += [(4,bytes((0x49,)),len(payloads)-1),
                         (64,sound(3 if active_failure else 1),None)]
    elif replace:
        payloads = [initial,payload(replacement_ids,**final_options)]
        commands = [(64,sound(order[0]),None)]
        if stop:
            commands += [(4,sound(128),None)]
        commands += [(4,bytes((0x49,)),1),(64,sound(order[1]),None)]
    else:
        payloads = [initial]
        for index,song in enumerate(order):
            if repeat_upload and index:
                commands += [(4 if active else 64,bytes((0x49,)),0)]
            commands += [(64 if not index or repeat_upload or not active else 6,sound(song),None)]
    return build_cartridge(payloads,commands)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--ids',default='2,10')
    parser.add_argument('--replacement-ids',default='10,2')
    parser.add_argument('--order',default='1')
    parser.add_argument('--score-profile',choices=SCORE_PROFILES,default='switch')
    parser.add_argument('--replacement-score-profile',choices=SCORE_PROFILES)
    parser.add_argument('--profile',choices=PROFILES,default='distinct')
    parser.add_argument('--fault',choices=FAULTS)
    parser.add_argument('--chunking',choices=CHUNKS,default='full')
    for flag in ('active','repeat-upload','replace','recover','active-failure','repeat-failure','stop'):
        parser.add_argument('--'+flag,action='store_true')
    args = parser.parse_args()
    try:
        image = build(tuple(int(value) for value in args.order.split(',')),
                      ids=tuple(int(value) for value in args.ids.split(',')),
                      replacement_ids=tuple(int(value) for value in args.replacement_ids.split(',')),
                      score_profile=args.score_profile,profile=args.profile,fault=args.fault,
                      chunking=args.chunking,active=args.active,repeat_upload=args.repeat_upload,
                      replace=args.replace,recover=args.recover,active_failure=args.active_failure,
                      repeat_failure=args.repeat_failure,stop=args.stop,
                      replacement_score_profile=args.replacement_score_profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
