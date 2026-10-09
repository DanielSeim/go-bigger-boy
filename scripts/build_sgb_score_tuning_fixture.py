#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned exact-octave tuning fixtures and bounded physical upload lifecycle."""
import argparse
import hashlib
from pathlib import Path
import struct
from build_sgb_score_atomic_fixture import build_cartridge
from build_sgb_score_mapping_fixture import objects as mapping_objects, checked_ids
from build_sgb_score_instrument_profiles_fixture import SCORE_PROFILES, PROFILES

CHUNKS = ('full','split','reverse','interleave','tail')
FAULTS = ('duplicate','unknown-third','reserved','mapping-only',
          'tuning2-3','tuning3-3','tuning2-255','tuning3-255','note-low','note-high')
SCORES = (*SCORE_PROFILES, 'octave')


def checked_tuning(tuning):
    if not isinstance(tuning,(tuple,list)) or len(tuning) != 2 or any(
            type(value) is not int or value not in (0,1,2) for value in tuning):
        raise ValueError('requires two bounded tuning selectors 0..2')
    return tuple(tuning)


def objects(ids=(2,10), *, score_profile='switch', profile='distinct', fault=None,
            tuning=(2,0), octave_slots=(2,3)):
    ids,tuning = checked_ids(ids),checked_tuning(tuning)
    if score_profile not in SCORES or fault not in (None,*FAULTS):
        raise ValueError('unknown tuning score or fault')
    if not isinstance(octave_slots,(tuple,list)) or len(octave_slots) != 2 or any(
            type(slot) is not int or slot not in (2,3) for slot in octave_slots):
        raise ValueError('requires two owned octave slots')
    inherited_fault = fault if fault in ('duplicate','unknown-third','reserved','mapping-only') else None
    score,data = mapping_objects(ids,score_profile='switch' if score_profile == 'octave' else score_profile,
                                 profile=profile,fault=inherited_fault)
    score,data = bytearray(score),bytearray(data)
    data[18],data[22] = tuning
    if score_profile == 'octave':
        if fault == 'unknown-third':
            raise ValueError('third-song fault uses the preceding multi-song score')
        score = bytearray(2048)
        struct.pack_into('<3H',score,0,0x2B20,0x2B30,0x2B40)
        for root in (0x20,0x30,0x40):
            struct.pack_into('<3H',score,root,0x2B60,0x2B70,0)
        for pattern,notes in enumerate((range(24,31),range(31,37))):
            for channel in (2,3):
                start = 0x100+0x100*pattern+0x80*(channel-2)
                struct.pack_into('<H',score,0x60+16*pattern+2*channel,0x2B00+start)
                stream = bytearray()
                if pattern == 0:
                    stream.extend((0xE0,ids[octave_slots[channel-2]-2],0xE1,10,0xED,127))
                    if channel == 2:
                        stream.extend((0xE5,160,0xE7,96))
                stream.extend((16,127,*(0x80+note for note in notes),0))
                score[start:start+len(stream)] = stream
    if fault and fault.startswith('tuning'):
        data[18 if fault[6] == '2' else 22] = int(fault.split('-')[1])
    if fault in ('note-low','note-high'):
        if score_profile != 'octave':
            raise ValueError('note faults require octave score')
        score[0x10C] = 0x97 if fault == 'note-low' else 0xA5
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
        raise ValueError('tuning payload exceeds physical frame')
    return result + bytes(4096-len(result))


def build(order=(1,), *, ids=(2,10), score_profile='switch', profile='distinct', tuning=(2,0), octave_slots=(2,3), replacement_tuning=None,
          fault=None, chunking='full', active=False, repeat_upload=False,
          replace=False, replacement_ids=(10,2), recover=False, active_failure=False,
          repeat_failure=False, stop=False, replacement_score_profile=None):
    ids,replacement_ids = checked_ids(ids),checked_ids(replacement_ids)
    tuning = checked_tuning(tuning)
    if replacement_tuning is not None and not (replace or recover):
        raise ValueError('replacement tuning requires replacement or recovery')
    replacement_tuning = tuning if replacement_tuning is None else checked_tuning(replacement_tuning)
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
    if replacement_score_profile is not None and (replacement_score_profile not in SCORES or
                                                  not (recover or replace)):
        raise ValueError('replacement score requires replacement or recovery')
    def sound(song):
        return bytes((0x41,0,0,0,song))
    options = dict(score_profile=score_profile,profile=profile,chunking=chunking,tuning=tuning,octave_slots=octave_slots)
    final_options = {**options,'score_profile':replacement_score_profile or score_profile,'tuning':replacement_tuning}
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
    parser.add_argument('--score-profile',choices=SCORES,default='switch')
    parser.add_argument('--replacement-score-profile',choices=SCORES)
    parser.add_argument('--profile',choices=PROFILES,default='distinct')
    parser.add_argument('--fault',choices=FAULTS)
    parser.add_argument('--chunking',choices=CHUNKS,default='full')
    for flag in ('active','repeat-upload','replace','recover','active-failure','repeat-failure','stop'):
        parser.add_argument('--'+flag,action='store_true')
    parser.add_argument('--tuning',default='2,0')
    parser.add_argument('--replacement-tuning')
    parser.add_argument('--octave-slots',default='2,3')
    args = parser.parse_args()
    try:
        image = build(tuple(int(value) for value in args.order.split(',')),
                      ids=tuple(int(value) for value in args.ids.split(',')),
                      replacement_ids=tuple(int(value) for value in args.replacement_ids.split(',')),
                      score_profile=args.score_profile,profile=args.profile,fault=args.fault,tuning=tuple(int(v) for v in args.tuning.split(',')),
                      replacement_tuning=None if args.replacement_tuning is None else tuple(int(v) for v in args.replacement_tuning.split(',')),
                      octave_slots=tuple(int(v) for v in args.octave_slots.split(',')),
                      chunking=args.chunking,active=args.active,repeat_upload=args.repeat_upload,
                      replace=args.replace,recover=args.recover,active_failure=args.active_failure,
                      repeat_failure=args.repeat_failure,stop=args.stop,
                      replacement_score_profile=args.replacement_score_profile)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
