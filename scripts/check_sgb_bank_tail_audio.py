#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audible recovery across single/repeated one-byte semantic upload tails."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_bank_recovery_audio_fixture import build,PROFILES
from check_sgb_bank_reject_audio import observe as rejection_observe
from check_sgb_bank_recovery_audio import compare
from check_sgb_bank_replace_audio import program,OPTIONS,IMAGE_HASH,RATE,MASTER


CASES=('tail','repeat-tail')


def observe(meta,raw,kind,profile,voice):
    if kind not in CASES or not isinstance(meta,dict) or meta.get('semantic_tail') is not True or (
            type(meta.get('repeated_rejection')) is not bool or meta['repeated_rejection']!=(kind=='repeat-tail')):
        raise ValueError('invalid one-byte semantic tail identity')
    count=5 if kind=='repeat-tail' else 3
    tokens=meta.get('consumed_tokens')
    if not isinstance(tokens,list) or len(tokens)!=count or any(type(t) is not int or t!=3 for t in tokens):
        raise ValueError('missing synchronized consumed IPL token')
    return rejection_observe(meta,raw,'bad-root',profile,voice,True,kind=='repeat-tail')


def fixture(kind,profile,voice):
    if kind not in CASES:raise ValueError('requires admitted semantic tail case')
    return build('bad-root',profile,voice,semantic_tail=True,repeat=kind=='repeat-tail')


def run(probe,host,directory,model,voice,kind,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json';image=fixture(kind,profile,voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(voice),'bad-root',kind],
        capture_output=True,text=True,timeout=150)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded semantic tail probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>1000000 or report.stat().st_size>16384:raise ValueError('semantic tail output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());obs=observe(meta,raw,kind,profile,voice)
    return obs,dict(model=model,voice=voice,kind=kind,profile=profile,mode=mode,
        fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe,kinds=CASES):
    if not isinstance(kinds,tuple) or not kinds or len(set(kinds))!=len(kinds) or any(k not in CASES for k in kinds):
        raise ValueError('requires distinct admitted semantic tail cases')
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA firmware changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-bank-tail-audio-') as name:
        d=Path(name);host=d/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in kinds:
                    controls={}
                    for profile in PROFILES:
                        controls[profile],summary=run(probe,host,d,model,voice,kind,profile,'combined');runs.append(summary)
                    comparisons.append(dict(model=model,voice=voice,kind=kind,**compare(controls,model,voice)))
                    scalar,summary=run(probe,host,d,model,voice,kind,'both','scalar');runs.append(summary)
                    if scalar!=controls['both']:raise ValueError('scalar semantic tail PCM/timeline differs')
    return dict(schema='gbb-sgb-bank-tail-audio-v1',qualification=False,playback=False,
        evidence='owned_48k_one_byte_semantic_tail_recovery',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--case',choices=CASES,help='run one complete semantic tail matrix; default runs both')
    args=parser.parse_args()
    try:result=measure(args.probe.resolve(),(args.case,) if args.case else CASES)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))
