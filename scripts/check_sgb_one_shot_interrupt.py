#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Actual stereo PCM at SOUND interruption and fresh one-shot restart."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_one_shot_interrupt_fixture import build,PROFILES
from build_sgb_score_transport import build as program
from check_sgb_host_sample_pitch import OPTIONS,IMAGE_HASH
from check_sgb_instrument_chromatic_reference import PITCHES
from check_sgb_polyphony_audio import SUM_TOLERANCE
from check_sgb_audio_transitions import RATE,MASTER,FRAMES,pitch


def observe(meta,raw,profile,held_voice):
    expected=dict(schema='gbb-sgb-one-shot-interrupt-v1',qualification=False,playback=False,
                  reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=4,version=0xDA,
                  restore_edges=15,restore_releases=15,restore_zeros=15,queued_onsets=15,queued_offs=15)
    if profile not in PROFILES or type(held_voice) is not int or held_voice not in (2,3) or not isinstance(meta,dict) or any(
            type(meta.get(k)) is not type(v) or meta[k]!=v for k,v in expected.items()):
        raise ValueError('invalid interruption identity or replay coverage')
    bounds=dict(frames=(100000,150000),clocks=(55000000,55000100),restore_count=(1,1024),
                pending_restores=(1,1024),gb_samples=(1,150000),native_samples=(1,150000))
    if any(type(meta.get(k)) is not int or not lo<=meta[k]<=hi for k,(lo,hi) in bounds.items()) or (
            meta['pending_restores']>meta['restore_count'] or not isinstance(raw,bytes) or len(raw)!=meta['frames']*4 or
            meta['frames']!=meta['clocks']*RATE//MASTER):
        raise ValueError('invalid interruption PCM or clock')
    pcm=list(struct.iter_unpack('<hh',raw))
    if any(v in (-32768,32767) for sample in pcm for v in sample):raise ValueError('clipped interruption PCM')
    edges=meta.get('edges');notes=meta.get('notes');completions=meta.get('completions')
    if not isinstance(edges,list) or len(edges)!=4:raise ValueError('missing SOUND marker')
    previous=-1
    for stage,edge in enumerate(edges,1):
        if not isinstance(edge,list) or len(edge)!=4 or any(type(x) is not int for x in edge) or (
                edge[:2]!=[stage,0 if stage==4 else 16]) or not previous<edge[2]<meta['frames'] or (
                not 0<=edge[3]*RATE//MASTER-edge[2]<=2):
            raise ValueError('invalid SOUND frame or clock')
        previous=edge[2]
    if not isinstance(notes,list) or len(notes)!=4 or not isinstance(completions,list) or len(completions)!=4:
        raise ValueError('missing paired restart notes or completions')
    by_voice={2:[],3:[]};mask=0
    for index,(note,completion) in enumerate(zip(notes,completions)):
        if not isinstance(note,list) or len(note)!=11 or any(type(x) is not int for x in note) or (
                note[0] not in by_voice or note[1]!=note[0]) or not 0<note[3]<note[4]<=note[5] or (
                not 0<=note[9]<=127 or note[10] not in (0,1<<note[0])):
            raise ValueError('invalid note identity or release')
        if not isinstance(completion,list) or len(completion)!=4 or any(type(x) is not int for x in completion):
            raise ValueError('invalid terminal observation')
        sequence=len(by_voice[note[0]])
        if sequence>1:raise ValueError('unexpected duplicate onset')
        start,stop=edges[2*sequence:2*sequence+2]
        if not start[2]<note[6]<note[7]<=note[8]<stop[2]+2048 or note[6]>=stop[2]:
            raise ValueError('onset outside SOUND lifecycle')
        target=PITCHES[2][9 if note[0]==held_voice else 0]
        if note[2]!=target:raise ValueError('wrong loop or transient pitch')
        if sequence==0:
            if not note[6]<stop[2]<=note[7]<stop[2]+960 or not note[9] or any(completion):
                raise ValueError('stop missed active first onset')
            if note[0]!=held_voice and note[10]:raise ValueError('first one-shot ended before stop')
        elif note[0]!=held_voice:
            end_half,end_frame,zero_half,zero_frame=completion
            if not note[3]<end_half<=zero_half<note[4] or not note[6]<end_frame<=zero_frame<note[7] or (
                    zero_frame-note[6]>768 or zero_frame-end_frame>32):
                raise ValueError('restarted transient did not complete before gate release')
            mask|=1<<index
        elif any(completion):raise ValueError('loop reported as one-shot')
        by_voice[note[0]].append((note,completion))
    if any(len(v)!=2 for v in by_voice.values()) or any(
            type(meta.get(k)) is not int or meta[k]!=mask for k in ('restore_ends','restore_natural_zeros')):
        raise ValueError('incomplete restart or natural-completion replay coverage')
    if any(a or b for a,b in pcm[edges[3][2]+3072:]):raise ValueError('final stop/mute leaves stale audio')
    return dict(meta=meta,left=[x[0] for x in pcm],right=[x[1] for x in pcm],
                held=by_voice[held_voice],peer=by_voice[5-held_voice])


def compare(controls,model,held_voice):
    both=controls['both'];meta=both['meta']
    for profile in PROFILES[1:]:
        other=controls[profile]
        if any(meta[k]!=other['meta'][k] for k in ('frames','clocks','edges','notes','completions','gb_samples','native_samples')):
            raise ValueError('interruption source controls have different timelines')
        if other['left']!=both['left']:raise ValueError('SOUND interruption disturbs GB PCM')
    if any(controls['gb']['right']):raise ValueError('silent SNES control is audible')
    maxima={}
    for channel in ('left','right'):
        maximum=max(abs(a-b-c+d) for a,b,c,d in zip(both[channel],controls['voice2'][channel],controls['voice3'][channel],controls['gb'][channel]))
        if maximum>SUM_TOLERANCE:raise ValueError('interrupted source sum differs')
        maxima[channel]=maximum
    held=controls[f'voice{held_voice}'];peer=controls[f'voice{5-held_voice}']
    first,second=(n for n,c in both['peer']);completion=both['peer'][1][1]
    transients=[]
    for index,note in enumerate((first,second)):
        begin=note[6];end=note[8]+32 if index==0 else completion[3]+32
        values=peer['right'][begin:end];active=[i for i,v in enumerate(values) if v]
        if len(active)<(16 if index==0 else 32):
            raise ValueError('interrupted or restarted transient is silent')
        if active[0]>128 or active[-1]>768 or max(abs(v) for v in values)<100:
            raise ValueError('transient onset or duration is implausible')
        transients.append(dict(on_frame=begin,first_audible_frame=begin+active[0],last_audible_frame=begin+active[-1],
                               nonzero_frames=len(active),peak=max(abs(v) for v in values),gate_off_frame=note[7]))
    window=peer['right'][max(first[6],first[7]-32):first[7]]
    if sum(v!=0 for v in window)<8 or max(abs(v) for v in window)<100:
        raise ValueError('SOUND stop did not interrupt audible transient PCM')
    if transients[0]['last_audible_frame']-first[6]+16>=transients[1]['last_audible_frame']-second[6]:
        raise ValueError('stopped transient not shorter than fresh natural playback')
    # Both native voices must settle after the first global stop, while GB runs.
    begin=max(n[0][8] for n in (both['held'][0],both['peer'][0]))+32
    end=min(both['held'][1][0][6],second[6])-4
    if end-begin<2048 or any(both['right'][begin:end]):raise ValueError('SOUND stop gap contains stale SNES audio')
    quiet=completion[3]+32;quiet_end=min(second[7],both['held'][1][0][7])-4
    if quiet_end-quiet<2048 or any(peer['right'][quiet:quiet_end]) or (
            both['right'][quiet:quiet_end]!=held['right'][quiet:quiet_end]) or max(abs(v) for v in held['right'][quiet:quiet_end])<100:
        raise ValueError('restarted transient leaves stale PCM or silences looping peer')
    start=second[6]+32
    held_pitch=pitch(held['right'][start:start+FRAMES],440)
    window=both['left'][begin:begin+FRAMES];midpoint=(max(window)+min(window))//2
    gb=pitch([v-midpoint for v in window],(MASTER/5 if model=='sgb' else 4194304)/8192)
    return dict(maximum_sum_error_lsb=maxima,sum_tolerance_lsb=SUM_TOLERANCE,transients=transients,
                stop_gap=[begin,end],natural_quiet=[quiet,quiet_end],held_pitch=held_pitch,gb_pulse=gb,
                matched_timeline=True,uninterrupted_gb_equal=True,final_silent=True)


def run(probe,host,directory,model,held_voice,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json'
    image=build(profile,held_voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(held_voice)],
                         capture_output=True,text=True,timeout=90)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded interrupt probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>600000 or report.stat().st_size>8192:raise ValueError('interruption output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());observed=observe(meta,raw,profile,held_voice)
    return observed,dict(model=model,held_voice=held_voice,profile=profile,mode=mode,
                         fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA image changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-one-shot-interrupt-') as name:
        directory=Path(name);host=directory/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for held_voice in (2,3):
                controls={}
                for profile in PROFILES:
                    controls[profile],summary=run(probe,host,directory,model,held_voice,profile,'combined');runs.append(summary)
                comparisons.append(dict(model=model,held_voice=held_voice,**compare(controls,model,held_voice)))
                scalar,summary=run(probe,host,directory,model,held_voice,'both','scalar')
                if scalar!=controls['both']:raise ValueError('scalar interruption PCM or timeline differs')
                runs.append(summary)
    return dict(schema='gbb-sgb-one-shot-interrupt-v1',qualification=False,playback=False,
                evidence='owned_48k_whole_host_audible_interruption',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(report,indent=2))
