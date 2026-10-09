#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned whole-host stereo continuity across SOUND and GB routing transitions."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_audio_transition_fixture import build,PROFILES,ROUTES
from build_sgb_score_transport import build as program
from check_sgb_host_sample_pitch import OPTIONS,IMAGE_HASH
from check_sgb_instrument_chromatic_reference import PITCHES
from check_sgb_owned_sample_pitch import audible_pitch,TOLERANCE_CENTS

RATE=48000
MASTER=21477273
FRAMES=2048
WARMUP=128
CASES=((2,2),(3,10))


def digest(values):
    return hashlib.sha256(struct.pack('<'+'h'*len(values),*values)).hexdigest()


def pitch(values,target):
    measured=audible_pitch(values,frames=FRAMES,warmup=WARMUP,sample_rate=RATE)
    cents=1200*math.log2(measured['frequency_hz']/target)
    if abs(cents)>TOLERANCE_CENTS:
        raise ValueError('transition pitch exceeds tolerance')
    return dict(target_hz=target,error_cents=cents,**measured)


def observe(meta,raw,profile,voice,instrument):
    expected=dict(schema='gbb-sgb-audio-transition-v1',qualification=False,playback=False,
                  reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=4,version=0xDA)
    if profile not in PROFILES or (voice,instrument) not in CASES or not isinstance(meta,dict) or any(
            type(meta.get(k)) is not type(v) or meta[k]!=v for k,v in expected.items()):
        raise ValueError('invalid transition identity or replay metadata')
    bounds=dict(frames=(100000,150000),clocks=(55000000,55000100),restore_count=(1,1024),
                gb_samples=(1,150000),native_samples=(1,150000))
    if any(type(meta.get(k)) is not int or not lo<=meta[k]<=hi for k,(lo,hi) in bounds.items()) or (
            not isinstance(raw,bytes) or len(raw)!=meta['frames']*4 or meta['frames']!=meta['clocks']*RATE//MASTER):
        raise ValueError('invalid PCM size, counter or clock')
    pcm=list(struct.iter_unpack('<hh',raw))
    if any(a in (-32768,32767) or b in (-32768,32767) for a,b in pcm):
        raise ValueError('hard-clipped transition capture')
    edges=meta.get('edges')
    if not isinstance(edges,list) or len(edges)!=6:
        raise ValueError('missing transition edge')
    previous_frame=previous_clock=-1
    for stage,(edge,route) in enumerate(zip(edges,ROUTES),1):
        expected_route=route if profile=='toggle' else 16 if profile=='on' else 0
        if not isinstance(edge,list) or len(edge)!=4 or any(type(x) is not int for x in edge) or (
                edge[:2]!=[stage,expected_route]) or not previous_frame<edge[2]<meta['frames'] or (
                not previous_clock<edge[3]<meta['clocks']) or not 0<=edge[3]*RATE//MASTER-edge[2]<=2:
            raise ValueError('wrong routing or discontinuous edge clock')
        previous_frame,previous_clock=edge[2:]
    notes=meta.get('notes')
    if not isinstance(notes,list) or len(notes)!=2:
        raise ValueError('missing held-tone onset')
    right=[x[1] for x in pcm]
    left=[x[0] for x in pcm]
    delta=[a-b for a,b in pcm]
    if profile=='silent' and any(delta):
        raise ValueError('silent GB control is asymmetric')
    measurements=[]
    for index,(note,start_stage,stop_stage) in enumerate(zip(notes,(1,5),(4,6))):
        if not isinstance(note,list) or len(note)!=7 or any(type(x) is not int for x in note) or (
                note[:3]!=[voice,2 if instrument==2 else 3,PITCHES[2][9]]) or not 0<note[3]<note[4]<=note[5] or (
                not edges[start_stage-1][2]<note[6]<edges[stop_stage-1][2]):
            raise ValueError('wrong note identity or onset outside SOUND interval')
        # Compare physical APU half clocks to actual GB action clocks.
        off_clock=note[4]*MASTER//2048000
        if not edges[stop_stage-1][3]<=off_clock<edges[stop_stage-1][3]+MASTER//50:
            raise ValueError('key-off does not follow authored SOUND stop')
        begin=note[6]+128
        if begin+FRAMES>=edges[stop_stage-1][2]:
            raise ValueError('insufficient held-tone capture before stop')
        measurements.append(pitch(right[begin:begin+FRAMES],440))
    # A settled window in the stop gap, and the complete final silent tail.
    for begin,end in ((edges[3][2]+1024,edges[4][2]),(edges[5][2]+3072,len(pcm))):
        if begin>=end or any(right[begin:end]):
            raise ValueError('SNES source persists after SOUND stop')
    # DMG high-pass coefficient .999958 per GB clock: 58.7 ms reduces even a
    # full int16 initial capacitor offset below one combined-output LSB on
    # either model with the default half GB gain.
    # Two LSBs cover the subtraction of separately rounded stereo mixer values.
    if profile=='toggle':
        for stage in (2,6):
            begin=edges[stage-1][2]+2816
            if max(abs(x) for x in delta[begin:begin+128])>2:
                raise ValueError('GB routing mute leaves stale audio')
    return dict(right=right,left=left,delta=delta,measurements=measurements,
                right_sha256=digest(right),left_sha256=digest(left))


def compare(controls,model):
    toggle,silent,on=(controls[k] for k in PROFILES)
    if toggle['right']!=silent['right'] or toggle['right']!=on['right']:
        raise ValueError('GB routing changes uninterrupted SNES PCM')
    meta=toggle['meta']
    for control in (silent,on):
        other=control['meta']
        if any(meta[k]!=other[k] for k in ('frames','clocks','notes','gb_samples','native_samples')) or (
                [e[2:] for e in meta['edges']]!=[e[2:] for e in other['edges']]):
            raise ValueError('source controls do not share an output timeline')
    pulses=[]
    maximum_derivative=0
    for stage in (1,3,4,5):
        begin=meta['edges'][stage-1][2]+512
        values=toggle['delta'][begin:begin+FRAMES]
        midpoint=(max(values)+min(values))//2
        pulses.append(pitch([x-midpoint for x in values],(MASTER/5 if model=='sgb' else 4194304)/8192))
        # Routing changes capacitor DC, but must retain pulse edge phase.
        # The identical right channel cancels. Two LSBs cover differencing
        # rounded left outputs; two cover capacitor-offset slew for this
        # bounded volume-4 pulse after the 512-frame settling interval.
        residual=[a-b for a,b in zip(values,on['delta'][begin:begin+FRAMES])]
        deviation=max(abs(b-a) for a,b in zip(residual,residual[1:]))
        if deviation>4:
            raise ValueError('GB unmute loses pulse phase or duplicates a sample')
        maximum_derivative=max(maximum_derivative,deviation)
    return dict(gb_pulses=pulses,maximum_phase_residual_step_lsb=maximum_derivative,
                uninterrupted_snes_equal=True,settled_stop_silent=True,settled_gb_mute_silent=True)


def run(probe,host,directory,model,voice,instrument,profile,mode):
    game=directory/'game.gb'; raw_path=directory/'output.pcm'; meta_path=directory/'output.json'
    game.write_bytes(build(profile,voice,instrument))
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(raw_path),str(meta_path)],
                         capture_output=True,text=True,timeout=90)
    if child.returncode or child.stdout or len(child.stderr)>1024:
        raise ValueError('transition probe failed: '+child.stderr[:256])
    if raw_path.stat().st_size>600000 or meta_path.stat().st_size>8192:
        raise ValueError('transition capture exceeds output bound')
    meta=json.loads(meta_path.read_text()); raw=raw_path.read_bytes()
    observed=observe(meta,raw,profile,voice,instrument)
    observed['meta']=meta
    return observed,dict(model=model,voice=voice,instrument=instrument,profile=profile,mode=mode,
                         fixture_sha256=hashlib.sha256(game.read_bytes()).hexdigest(),
                         pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta,
                         right_pcm_sha256=observed['right_sha256'],left_pcm_sha256=observed['left_sha256'],
                         snes_pitch=observed['measurements'])


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:
        raise ValueError('DA image changed')
    runs=[]; comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-audio-transition-') as name:
        directory=Path(name); host=directory/'host.rom'; host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice,instrument in CASES:
                controls={}
                for profile in PROFILES:
                    controls[profile],summary=run(probe,host,directory,model,voice,instrument,profile,'combined')
                    runs.append(summary)
                comparisons.append(dict(model=model,voice=voice,instrument=instrument,**compare(controls,model)))
                scalar,summary=run(probe,host,directory,model,voice,instrument,'toggle','scalar')
                if scalar!=controls['toggle']:
                    # Restore counts are diagnostics of scheduling, not PCM.
                    for result in (scalar,controls['toggle']): result['meta'].pop('restore_count')
                    if scalar!=controls['toggle']:
                        raise ValueError('scalar transition PCM or timeline differs')
                runs.append(summary)
    return dict(schema='gbb-sgb-audio-transitions-v1',qualification=False,playback=False,
                evidence='owned_48k_whole_host_source_transitions',image_sha256=IMAGE_HASH,
                tolerance_cents=TOLERANCE_CENTS,runs=runs,comparisons=comparisons,
                maximum_snes_error_cents=max(abs(p['error_cents']) for r in runs for p in r['snes_pitch']),
                maximum_gb_error_cents=max(abs(p['error_cents']) for c in comparisons for p in c['gb_pulses']))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:
        report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:
        parser.error(str(error))
    print(json.dumps(report,indent=2))
