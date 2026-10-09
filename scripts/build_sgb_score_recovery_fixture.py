#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned failed-upload/SOUND/retry sequences through actual GB VRAM."""
import argparse
import hashlib
import struct
from pathlib import Path
from build_sgb_score_atomic_fixture import build_cartridge, payload, FAULTS, VALID


def build(kind='asset-gap', *, cold=False, repeat=False, stop=False, recover=True,
          mixed=False, final='interleave', invalid_root=None, semantic_tail=False):
    if kind not in FAULTS or final not in VALID:
        raise ValueError('unknown failure or final transaction')
    if any(type(flag) is not bool for flag in (cold, repeat, stop, recover, mixed, semantic_tail)):
        raise ValueError('fixture flags must be boolean')
    if mixed and (not cold or repeat or stop or not recover):
        raise ValueError('mixed failures require cold recovery without repeat/stop')
    if invalid_root is not None and (type(invalid_root) is not int or
                                    invalid_root not in (1,2,3) or
                                    kind != 'bad-content' or mixed):
        raise ValueError('invalid root requires semantic failure and root 1..3 without mixed failures')
    if semantic_tail and (kind != 'bad-content' or mixed):
        raise ValueError('semantic tail requires semantic failure without mixed failures')
    sound = bytes((0x41,0,0,0,1 if cold else 3))
    bad = payload(kind,replacement=True)
    if invalid_root is not None:
        changed = bytearray(payload('full',replacement=True))
        struct.pack_into('<H',changed,4+2*(invalid_root-1),0)
        bad = bytes(changed)
    if semantic_tail:
        # A one-byte final chunk leaves IPL jump token 3, which can alias the
        # third host loader request unless semantic rejection synchronizes it.
        data = (bad[:2052] + struct.pack('<HH',191,0x5000) + bad[2056:2247] +
                struct.pack('<HH',1,0x50BF) + bad[2247:2248] +
                struct.pack('<HH',0,0x0400))
        bad = data + bytes(4096-len(data))
    good = payload(final,replacement=True)
    if mixed:
        other = payload('asset-gap' if kind == 'bad-content' else 'bad-content',replacement=True)
        return build_cartridge((bad,other,good),
            ((64,sound,None),(4,bytes((0x49,)),1),(16,sound,None),
             (4,bytes((0x49,)),2),(64,sound,None)))
    if cold:
        payloads = [bad,good]
        commands = [(64,sound,None)]
        bad_index, good_index = 0,1
    else:
        payloads = [payload(),bad,good]
        commands = [(64,bytes((0x41,0,0,0,1)),None),
                    (4,bytes((0x49,)),1),(16,sound,None)]
        bad_index, good_index = 1,2
    if stop:
        commands += [(4,bytes((0x41,0,0,0,128)),None)]
    if repeat:
        commands += [(4,bytes((0x49,)),bad_index),(16,sound,None)]
    if recover:
        commands += [(4,bytes((0x49,)),good_index),(64,sound,None)]
    return build_cartridge(payloads,commands)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--kind',choices=FAULTS,default='asset-gap')
    parser.add_argument('--final',choices=VALID,default='interleave')
    parser.add_argument('--cold',action='store_true')
    parser.add_argument('--repeat',action='store_true')
    parser.add_argument('--stop',action='store_true')
    parser.add_argument('--no-recover',action='store_true')
    parser.add_argument('--mixed',action='store_true')
    parser.add_argument('--invalid-root',type=int,choices=(1,2,3))
    parser.add_argument('--semantic-tail',action='store_true')
    args = parser.parse_args()
    try:
        image = build(args.kind,cold=args.cold,repeat=args.repeat,stop=args.stop,
                      recover=not args.no_recover,mixed=args.mixed,final=args.final,
                      invalid_root=args.invalid_root,semantic_tail=args.semantic_tail)
        with args.output.open('xb') as output:
            output.write(image)
    except (OSError,ValueError) as error:
        parser.error(str(error))
    print(hashlib.sha256(image).hexdigest())
