#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure actual 48-kHz combined PCM with silent and active owned GB audio."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_combined_sample_pitch_fixture import build
from build_sgb_score_transport import build as program
from check_sgb_host_sample_pitch import OPTIONS,IMAGE_HASH,observe as octave_observe
from check_sgb_owned_sample_pitch import audible_pitch,TOLERANCE_CENTS

FRAMES=3072
WARMUP=192
RATE=48000


def observe(result,voice,instrument,reversed_map,model,active_gb):
    if model not in ('sgb','sgb2') or type(active_gb) is not bool:
        raise ValueError('invalid combined source identity')
    rows=octave_observe(result,voice,instrument,reversed_map,sample_rate=RATE,frames=FRAMES,warmup=WARMUP)
    changes=result.get('envelope_setup_changes')
    if not isinstance(changes,list) or len(changes)!=13 or any(type(x) is not int or x!=0 for x in changes):
        raise ValueError('changed voice setup inside combined notes')
    acoustic=result['acoustic']
    if type(acoustic.get('gb_samples_captured')) is not int or not 1<=acoustic['gb_samples_captured']<=250000 or (
            type(acoustic.get('clipped_samples')) is not int or acoustic['clipped_samples']!=0):
        raise ValueError('missing GB source or clipped combined output')
    target=(21477273/5 if model=='sgb' else 4194304)/8192
    for row,window,note in zip(rows,result['acoustic']['windows'],result['envelope_notes']):
        start,end=window.get('start_half'),window.get('end_half')
        if type(start) is not int or type(end) is not int or not note[16]<=start<end<note[17]:
            raise ValueError('combined window crosses observed gate')
        left=window.get('left')
        if not isinstance(left,list) or len(left)!=FRAMES or any(type(x) is not int or not -32768<x<32767 for x in left):
            raise ValueError('malformed or clipped combined left PCM')
        delta=[a-b for a,b in zip(left,window['pcm'])]
        if not active_gb:
            if any(delta):
                raise ValueError('silent GB output is asymmetric')
        else:
            # Known constant pulse: midpoint of its two levels removes DAC DC.
            # Only the isolated channel difference is centered, never SNES PCM.
            midpoint=(min(delta)+max(delta))//2
            centered=[x-midpoint for x in delta]
            measured=audible_pitch(centered,frames=FRAMES,warmup=WARMUP,sample_rate=RATE)
            cents=1200*math.log2(measured['frequency_hz']/target)
            if abs(cents)>TOLERANCE_CENTS:
                raise ValueError('active GB pulse pitch exceeds tolerance')
            row['gb_pulse']={'target_hz':target,'error_cents':cents,**measured}
        row['left_pcm_sha256']=hashlib.sha256(struct.pack('<'+'h'*FRAMES,*left)).hexdigest()
        row.update(start_half=start,end_half=end)
    return rows


def run(probe,host,game,model,voice,instrument,reversed_map,active_gb,mode):
    fixture=build(voice,instrument,reversed_map,active_gb)
    game.write_bytes(fixture)
    child=subprocess.run([str(probe),str(host),str(game),model,'70000000',mode],capture_output=True,text=True,timeout=90)
    if child.returncode or len(child.stdout)>768*1024:
        raise ValueError('bounded combined PCM probe failed: '+child.stderr[:256])
    result=json.loads(child.stdout)
    rows=observe(result,voice,instrument,reversed_map,model,active_gb)
    return result,dict(model=model,voice=voice,instrument=instrument,reversed_map=reversed_map,
                       active_gb=active_gb,mode=mode,restore_count=result['restore_count'],
                       reset_equal=True,restore_equal=True,fixture_sha256=hashlib.sha256(fixture).hexdigest(),
                       pcm=result['pcm'],gb_samples_captured=result['acoustic']['gb_samples_captured'],
                       clipped_samples=result['acoustic']['clipped_samples'],observations=rows)


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:
        raise ValueError('DA program image changed')
    runs=[]
    with tempfile.TemporaryDirectory(prefix='gbb-combined-pitch-') as directory:
        host,game=Path(directory)/'host.rom',Path(directory)/'game.gb'
        host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for active in (False,True):
                for voice in (2,3):
                    for instrument in (2,10):
                        for reverse in (False,True):
                            result,summary=run(probe,host,game,model,voice,instrument,reverse,active,'combined')
                            runs.append(summary)
                            if (voice,instrument,reverse)==(3,10,True):
                                scalar,summary=run(probe,host,game,model,voice,instrument,reverse,active,'scalar')
                                if any(result[key]!=scalar[key] for key in ('pcm','acoustic','envelope_notes')):
                                    raise ValueError('combined/scalar output differs')
                                runs.append(summary)
    return dict(schema='gbb-sgb-combined-sample-pitch-v1',qualification=False,playback=False,
                evidence='48k_combined_whole_host_owned_periodic_samples',image_sha256=IMAGE_HASH,
                sample_rate_hz=RATE,frames_per_window=FRAMES,warmup_frames=WARMUP,
                tolerance_cents=TOLERANCE_CENTS,runs=runs,
                maximum_absolute_error_cents=max(abs(n['error_cents']) for r in runs for n in r['observations']),
                maximum_gb_error_cents=max(abs(n['gb_pulse']['error_cents']) for r in runs if r['active_gb'] for n in r['observations']))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:
        report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps(report,indent=2))
