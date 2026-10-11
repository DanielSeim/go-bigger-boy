#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate measured voice-volume setup against opaque SGB1/SGB2 executions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_volume_fixture import build,CASES
from check_sgb_vendor_music_reference import capture

EXPECTED={'pan': [[7,7],[0,11],[11,0]],'volume': [[7,7],[1,1],[1,1]],
          'velocity': [[6,6]]*3}


def summarize(data,case,holdout=False):
    if case not in CASES or type(holdout) is not bool:
        raise ValueError('invalid voice-volume control')
    notes=[e for e in data['events'] if e['register']==0x4C]
    if (data['music']!=[1,2,3] or len(data['requests'])!=3 or
            len(notes)!=3 or [n['request'] for n in notes]!=[1,2,3] or
            [n['pitch'] for n in notes] != ([1437,1357,1281] if holdout else [717]*3) or
            any(n['value']!=4 or (n['srcn'],n['adsr1'],n['adsr2'],n['gain'])!=(2,255,224,184)
                for n in notes)):
        raise ValueError('owned voice-volume note setup changed')
    writes=data.get('selection_volume_writes')
    if not isinstance(writes,list) or not 6<=len(writes)<=512:
        raise ValueError('missing or excessive voice-volume writes')
    previous=-1
    for w in writes:
        if (not isinstance(w,dict) or any(type(w.get(k)) is not int for k in ('half','request','register','value')) or
                w['half']<previous or w['register'] not in (0x0C,0x1C,0x20,0x21) or
                not 0<=w['value']<=255 or not 1<=w['request']<=3 or
                not data['requests'][w['request']-1]<=w['half']<notes[w['request']-1]['half']):
            raise ValueError('invalid bounded volume write')
        previous=w['half']
    result=[]
    for i,note in enumerate(notes):
        selected=[w for w in writes if w['request']==i+1]
        voices=[]
        for register in (0x20,0x21):
            matches=[w for w in selected if w['register']==register]
            if not matches: raise ValueError('missing voice-volume side')
            voices.append(matches[-1]['value'])
        masters=[w for w in selected if w['register'] in (0x0C,0x1C)]
        result.append(dict(music=i+1,voice=voices,
            selection_ms=(note['half']-data['requests'][i])/2048,
            master_writes=len(masters),
            master_values=sorted(set(w['value'] for w in masters))))
    final=data.get('final')
    if (not isinstance(final,dict) or set(final)!={'flg','kof','echo_left','echo_right'} or
            any(type(v) is not int or v!=0 for v in final.values())):
        raise ValueError('voice-volume completion changed')
    return result


def check(data,case,holdout=False):
    result=summarize(data,case,holdout)
    if [r['voice'] for r in result]!=EXPECTED[case]:
        raise ValueError('measured voice-volume setup differs')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=firmware();runs=[]
        with tempfile.TemporaryDirectory(prefix='gbb-vendor-volume-') as temporary:
            root=Path(temporary);owned=root/'owned.rom';owned.write_bytes(image)
            inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom')):
                original=args.firmware_dir/name;cases=[]
                for case in CASES:
                    for holdout in (False,True):
                        fixture=build(case,holdout);game=root/'game.gb';game.write_bytes(fixture)
                        summaries=[check(capture(args.probe.resolve(),program,game,model,200000000,inputs,
                            selection_volume=True),case,holdout) for program in (original,owned)]
                        cases.append(dict(case=case,holdout=holdout,fixture_sha256=hashlib.sha256(fixture).hexdigest(),
                            original=summaries[0],replacement=summaries[1],voice_equal=True))
                runs.append(dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases=cases))
        print(json.dumps(dict(schema='gbb-sgb-vendor-volume-v1',qualification=False,
            firmware_sha256=hashlib.sha256(image).hexdigest(),runs=runs),sort_keys=True))
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.exit(2,'bounded voice-volume reference comparison failed\n')


if __name__=='__main__': main()
