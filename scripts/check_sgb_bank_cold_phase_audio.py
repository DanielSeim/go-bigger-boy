#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Subframe SOUND timing through cold mixed one-byte recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_bank_cold_phase_audio_fixture import build,PROFILES,ORDERS,failures,SPINS
from check_sgb_bank_cold_mixed_tail_audio import observe as tail_observe
from check_sgb_bank_cold_audio import compare as cold_compare,row
from check_sgb_bank_replace_audio import program,OPTIONS,IMAGE_HASH,RATE,MASTER


def observe(meta,raw,kind,profile,voice):
    result=tail_observe(meta,raw,kind,profile,voice)
    phases=meta.get('command_phases');model=meta.get('phase_model')
    if meta.get('command_phase')!='staggered' or model not in ('sgb','sgb2') or not isinstance(phases,list) or len(phases)!=8:
        raise ValueError('missing observed staggered command phase identity')
    previous=0
    for i,p in enumerate(phases):
        if not row(p,4) or p[0]!=i+1 or not previous<p[1]<=25000000 or not 0<=p[2]<154 or not 0<=p[3]<456:
            raise ValueError('invalid native GB cycle/LCD phase observation')
        elapsed=meta['edges'][i][3]-meta['edges'][0][3]
        cycles=elapsed//5 if model=='sgb' else elapsed*4194304//MASTER
        if abs(p[1]-phases[0][1]-cycles)>32:raise ValueError('GB phase clock does not match host timeline')
        if SPINS[i]:
            delay=28*SPINS[i]+8
            actual=(p[2]*456+p[3]-144*456)%70224
            if not delay<=actual<=delay+128:raise ValueError('SOUND did not execute at its subframe phase')
        previous=p[1]
    return result


def compare(controls,model,voice):
    for control in controls.values():
        if control['meta']['phase_model']!=model or control['meta']['command_phases']!=controls['both']['meta']['command_phases']:
            raise ValueError('source controls change observed command phases')
    result=cold_compare(controls,model,voice)
    return dict(**result,command_phases=controls['both']['meta']['command_phases'],spin_counts=list(SPINS))


fixture=build


def run(probe,host,directory,model,voice,kind,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json';image=fixture(kind,profile,voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(voice),failures(kind)[0],'cold-phase'],
        capture_output=True,text=True,timeout=150)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded mixed recovery probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>1000000 or report.stat().st_size>16384:raise ValueError('mixed recovery output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());obs=observe(meta,raw,kind,profile,voice)
    if meta['phase_model']!=model:raise ValueError('phase report model mismatch')
    return obs,dict(model=model,voice=voice,kind=kind,profile=profile,mode=mode,
        fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe,kinds=ORDERS):
    if not isinstance(kinds,tuple) or not kinds or len(set(kinds))!=len(kinds) or any(k not in ORDERS for k in kinds):
        raise ValueError('requires distinct admitted mixed recovery cases')
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA firmware changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-bank-cold-phase-audio-') as name:
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
    return dict(schema='gbb-sgb-bank-cold-phase-audio-v1',qualification=False,playback=False,
        evidence='owned_48k_staggered_cold_mixed_one_byte_recovery',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--order',choices=ORDERS,help='run one complete order matrix; default runs both')
    args=parser.parse_args()
    try:result=measure(args.probe.resolve(),(args.order,) if args.order else ORDERS)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))
