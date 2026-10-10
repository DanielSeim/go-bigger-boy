#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""First audible bank after cold mixed failures with a one-byte semantic tail."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_bank_cold_mixed_audio_fixture import build,PROFILES,ORDERS,failures
from check_sgb_bank_cold_audio import observe as cold_observe,compare
from check_sgb_bank_replace_audio import program,OPTIONS,IMAGE_HASH,RATE,MASTER


def observe(meta,raw,kind,profile,voice):
    first,_=failures(kind)
    if not isinstance(meta,dict) or meta.get('mixed_rejection') is not True:
        raise ValueError('invalid mixed recovery identity')
    return cold_observe(meta,raw,first,profile,voice,repeat=True,mixed=True,semantic_tail=True)


def fixture(kind,profile,voice):
    return build(kind,profile,voice,semantic_tail=True)


def run(probe,host,directory,model,voice,kind,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json';image=fixture(kind,profile,voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(voice),failures(kind)[0],'cold-mixed-tail'],
        capture_output=True,text=True,timeout=150)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded mixed recovery probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>1000000 or report.stat().st_size>16384:raise ValueError('mixed recovery output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());obs=observe(meta,raw,kind,profile,voice)
    return obs,dict(model=model,voice=voice,kind=kind,profile=profile,mode=mode,
        fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe,kinds=ORDERS):
    if not isinstance(kinds,tuple) or not kinds or len(set(kinds))!=len(kinds) or any(k not in ORDERS for k in kinds):
        raise ValueError('requires distinct admitted mixed recovery cases')
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA firmware changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-bank-cold-mixed-tail-audio-') as name:
        d=Path(name);host=d/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in kinds:
                    controls={}
                    for profile in PROFILES:
                        controls[profile],summary=run(probe,host,d,model,voice,kind,profile,'combined');runs.append(summary)
                    comparisons.append(dict(model=model,voice=voice,kind=kind,**compare(controls,model,voice)))
                    scalar,summary=run(probe,host,d,model,voice,kind,'both','scalar');runs.append(summary)
                    if scalar!=controls['both']:raise ValueError('scalar mixed recovery PCM/timeline differs')
    return dict(schema='gbb-sgb-bank-cold-mixed-tail-audio-v1',qualification=False,playback=False,
        evidence='owned_48k_cold_mixed_one_byte_failure_bank_recovery',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--order',choices=ORDERS,help='run one complete order matrix; default runs both')
    args=parser.parse_args()
    try:result=measure(args.probe.resolve(),(args.order,) if args.order else ORDERS)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))
