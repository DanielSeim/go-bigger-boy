#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fresh owned PCM after rejection, blocked SOUND and valid changed-bank retry."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_bank_recovery_audio_fixture import build,objects,FAULTS,PROFILES
from check_sgb_bank_reject_audio import observe as rejection_observe
from check_sgb_bank_replace_audio import compare as source_compare
from check_sgb_bank_replace_audio import program,OPTIONS,IMAGE_HASH,RATE,MASTER,FRAMES,pitch


def observe(meta,raw,kind,profile,voice):
    return rejection_observe(meta,raw,kind,profile,voice,True)


def compare(controls,model,voice):
    both=controls['both'];meta=both['meta']
    for profile in PROFILES[1:]:
        if any(meta[k]!=controls[profile]['meta'][k] for k in ('mute','gaps','rejects')) or (
                meta['recovered'][:16]!=controls[profile]['meta']['recovered'][:16]):
            raise ValueError('source controls change rejection/admission timeline')
    result=source_compare(controls,model,voice)
    pulses=[]
    for begin,limit in ((meta['rejects'][1][2]+128,meta['edges'][4][2]),
                        (meta['rejects'][2][2]+128,meta['edges'][5][2]),
                        (meta['clears'][2][1]+128,meta['edges'][6][2]),
                        (meta['notes'][1][6]+128,meta['edges'][7][2])):
        if begin+FRAMES>=limit:raise ValueError('GB recovery window crosses a command')
        values=both['left'][begin:begin+FRAMES];mid=(max(values)+min(values))//2
        pulses.append(pitch([x-mid for x in values],(MASTER/5 if model=='sgb' else 4194304)/8192))
    return dict(**result,gb_recovery_windows=pulses,blocked_sound_silent=True,
        valid_retry_admitted=True,fresh_restart_audible=True)


def run(probe,host,directory,model,voice,kind,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json';image=build(kind,profile,voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(voice),kind,'recover'],
        capture_output=True,text=True,timeout=150)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded recovery probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>1000000 or report.stat().st_size>16384:raise ValueError('recovery output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());obs=observe(meta,raw,kind,profile,voice)
    return obs,dict(model=model,voice=voice,kind=kind,profile=profile,mode=mode,
        fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe,kinds=FAULTS):
    if not isinstance(kinds,tuple) or not kinds or len(set(kinds))!=len(kinds) or any(k not in FAULTS for k in kinds):
        raise ValueError('requires distinct admitted recovery failures')
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA firmware changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-bank-recovery-audio-') as name:
        d=Path(name);host=d/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in kinds:
                    controls={}
                    for profile in PROFILES:
                        controls[profile],summary=run(probe,host,d,model,voice,kind,profile,'combined');runs.append(summary)
                    comparisons.append(dict(model=model,voice=voice,kind=kind,**compare(controls,model,voice)))
                    scalar,summary=run(probe,host,d,model,voice,kind,'both','scalar');runs.append(summary)
                    if scalar!=controls['both']:raise ValueError('scalar recovery PCM/timeline differs')
    return dict(schema='gbb-sgb-bank-recovery-audio-v1',qualification=False,playback=False,
        evidence='owned_48k_rejected_bank_fresh_recovery',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--kind',choices=FAULTS,help='run one complete failure matrix; default runs both')
    args=parser.parse_args()
    try:result=measure(args.probe.resolve(),(args.kind,) if args.kind else FAULTS)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))
