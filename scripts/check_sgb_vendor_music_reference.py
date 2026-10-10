#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare bounded title timing and owned uploaded bindings with opaque originals."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build, ROOT
from build_sgb_vendor_music_fixture import bank, payload, assets
from build_sgb_score_atomic_fixture import build_cartridge
from check_sgb_vendor_music_title import TITLE_SHA256, SCRIPT_SHA256


def capture(probe, program, game, model, clocks, inputs):
    child = subprocess.run([str(probe),str(program),str(game),model,str(clocks),str(inputs),'bundled'],
                           capture_output=True,timeout=240)
    if child.returncode or len(child.stdout)>32768:
        raise ValueError('bounded register-only observation failed')
    data = json.loads(child.stdout)
    if (data.get('schema')!='gbb-sgb-music-timeline-v1' or data.get('qualification') is not False or
            data.get('model')!=model or not isinstance(data.get('events'),list) or
            not isinstance(data.get('requests'),list) or len(data['events'])>128 or len(data['requests'])>16):
        raise ValueError('invalid observation identity or bounds')
    requests=data['requests']; previous=-1
    if not isinstance(data.get('music'),list) or len(data['music'])!=len(requests) or any(type(v) is not int or not 0<=v<=255 for v in data['music']):
        raise ValueError('invalid music request metadata')
    if any(type(t) is not int or t<0 for t in requests) or requests!=sorted(set(requests)):
        raise ValueError('invalid request ordering')
    for event in data['events']:
        if (not isinstance(event,dict) or any(type(event.get(k)) is not int for k in ('half','register','value','request')) or
                event['half']<previous or event['register'] not in (0x4C,0x5C,0x6C) or
                not 0<=event['value']<=255 or not 1<=event['request']<=len(requests) or
                event['half']<requests[event['request']-1]):
            raise ValueError('invalid register observation')
        previous=event['half']
        if event['register']==0x4C and (event['value']!=4 or any(type(event.get(k)) is not int or
            not 0<=event[k]<=limit for k,limit in (('pitch',65535),('srcn',255),('adsr1',255),('adsr2',255),('gain',255)))):
            raise ValueError('invalid key-on setup')
    return data


def title_summary(data):
    requests=data['requests']; events=data['events']
    notes=[e for e in events if e['register']==0x4C]
    if data['music']!=[0,1,1] or len(requests)!=3 or len(notes)!=2 or [e['request'] for e in notes]!=[2,3]:
        raise ValueError('title request/restart contract changed')
    gate=next((e for e in events if e['half']>notes[-1]['half'] and e['register']==0x5C and e['value']==4),None)
    stop=next((e for e in events if gate and e['half']>gate['half'] and e['register']==0x5C and e['value']==255),None)
    if not gate or not stop or any(e['register']==0x5C and e['value']==4 and e['request']==2 and notes[0]['half']<e['half']<notes[1]['half'] for e in events):
        raise ValueError('title gate/stop contract changed')
    ms=lambda halves:round(halves/2048,6)
    return {'request_spacing_ms':ms(requests[2]-requests[1]),
            'keyon_after_request_ms':[ms(e['half']-requests[e['request']-1]) for e in notes],
            'last_gate_ms':ms(gate['half']-notes[-1]['half']),
            'stop_after_last_keyon_ms':ms(stop['half']-notes[-1]['half']),
            'note_pitches':[e['pitch'] for e in notes], 'sources':[e['srcn'] for e in notes],
            'final_flg_write':next((e['value'] for e in reversed(events) if e['register']==0x6C),None),
            'first_request_interrupted':True}


def binding_game(descriptor):
    score=bytearray(bank(echo=False)); score[0x91]=2
    return build_cartridge((payload(((0x2B00,bytes(score)),)),payload(assets(descriptor=descriptor))),
                           ((4,bytes((0x49,)),1),(16,bytes((0x41,0,0,0,2)),None)))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--game',type=Path)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-vendor-registers-') as directory:
            base=Path(directory); replacement=base/'owned.rom'; replacement.write_bytes(build())
            inputs=base/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            reports=[]
            for model in ('sgb','sgb2'):
                original=args.firmware_dir/('sgb1.program.rom' if model=='sgb' else 'sgb2.program.rom')
                result={'model':model,'program_sha256':hashlib.sha256(original.read_bytes()).hexdigest()}
                pitches={}
                for name,descriptor in (('base',(2,0x8E,0xAF,0,16,0)),('half',(2,0x8E,0xAF,0,8,0)),('envelope',(2,0x8F,0x6F,48,16,0))):
                    game=base/(name+'.gb'); game.write_bytes(binding_game(descriptor))
                    for kind,program in (('original',original),('replacement',replacement)):
                        data=capture(args.probe.resolve(),program.resolve(),game,model,300000000,inputs)
                        notes=[e for e in data['events'] if e['register']==0x4C]
                        if len(notes)!=3 or any([e['srcn'],e['adsr1'],e['adsr2'],e['gain']]!=list(descriptor[:4]) for e in notes):
                            raise ValueError('uploaded source/envelope binding mismatch')
                        pitches[kind,name]=[e['pitch'] for e in notes]
                for kind in ('original','replacement'):
                    if pitches[kind,'base']!=pitches[kind,'envelope'] or any(abs(a/2-b)>1 for a,b in zip(pitches[kind,'base'],pitches[kind,'half'])):
                        raise ValueError('uploaded tuning scaling mismatch')
                result['binding']={kind:{name:pitches[kind,name] for name in ('base','half','envelope')} for kind in ('original','replacement')}
                if args.game:
                    script=ROOT/'tests/fixtures/sgb/titles/donkey-kong-gameplay.script'
                    if hashlib.sha256(args.game.read_bytes()).hexdigest()!=TITLE_SHA256 or hashlib.sha256(script.read_bytes()).hexdigest()!=SCRIPT_SHA256:
                        raise ValueError('private title/input identity mismatch')
                    result['title']={kind:title_summary(capture(args.probe.resolve(),program.resolve(),args.game.resolve(),model,1150000000,script))
                                     for kind,program in (('original',original),('replacement',replacement))}
                reports.append(result)
    except (OSError,ValueError,KeyError,TypeError,subprocess.TimeoutExpired):
        parser.error('private reference comparison failed; child diagnostics suppressed')
    print(json.dumps({'schema':'gbb-sgb-vendor-music-reference-v1','qualification':False,
                      'firmware_sha256':hashlib.sha256(build()).hexdigest(),'runs':reports},indent=2))


if __name__=='__main__': main()
