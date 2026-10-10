#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Audible owned loop/one-shot/loop selection through the real combined host."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_instrument_change_audio_fixture import build, PROFILES
from build_sgb_score_transport import build as program
from check_sgb_host_sample_pitch import OPTIONS, IMAGE_HASH
from check_sgb_polyphony_audio import SUM_TOLERANCE
from check_sgb_instrument_chromatic_reference import PITCHES
from check_sgb_audio_transitions import RATE, MASTER, FRAMES, pitch, digest


def observe(meta,raw,profile,held_voice):
    expected=dict(schema='gbb-sgb-instrument-change-audio-v1',qualification=False,playback=False,
                  reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=2,version=0xDA,
                  restore_edges=3,restore_releases=15,restore_zeros=15,queued_onsets=15,queued_offs=15)
    if profile not in PROFILES or type(held_voice) is not int or held_voice not in (2,3) or not isinstance(meta,dict) or any(
            type(meta.get(k)) is not type(v) or meta[k]!=v for k,v in expected.items()):
        raise ValueError('invalid instrument-change identity or lifecycle coverage')
    bounds=dict(frames=(100000,150000),clocks=(55000000,55000100),restore_count=(1,1024),
                pending_restores=(1,1024),gb_samples=(1,150000),native_samples=(1,150000))
    if any(type(meta.get(k)) is not int or not lo<=meta[k]<=hi for k,(lo,hi) in bounds.items()) or (
            meta['pending_restores']>meta['restore_count'] or not isinstance(raw,bytes) or len(raw)!=meta['frames']*4 or
            meta['frames']!=meta['clocks']*RATE//MASTER):
        raise ValueError('invalid instrument-change PCM, counters or clock')
    pcm=list(struct.iter_unpack('<hh',raw))
    if any(a in (-32768,32767) or b in (-32768,32767) for a,b in pcm):raise ValueError('hard-clipped output')
    edges=meta.get('edges')
    if not isinstance(edges,list) or len(edges)!=2:raise ValueError('missing lifecycle edge')
    previous=-1
    for stage,edge in enumerate(edges,1):
        if not isinstance(edge,list) or len(edge)!=4 or any(type(x) is not int for x in edge) or (
                edge[:2]!=[stage,16 if stage==1 else 0]) or not previous<edge[2]<meta['frames'] or (
                not 0<=edge[3]*RATE//MASTER-edge[2]<=2):raise ValueError('invalid lifecycle frame/clock')
        previous=edge[2]
    notes=meta.get('notes');completions=meta.get('completions')
    if not isinstance(notes,list) or len(notes)!=4 or not isinstance(completions,list) or len(completions)!=4:
        raise ValueError('expected held note and three instrument selections')
    by_voice={2:[],3:[]};mask=0;natural=None
    for index,(note,completion) in enumerate(zip(notes,completions)):
        if not isinstance(note,list) or len(note)!=11 or any(type(x) is not int for x in note) or (
                note[0] not in by_voice or note[1] not in (2,3) or not 0<note[3]<note[4]<=note[5] or (
                not edges[0][2]<note[6]<note[7]<=note[8]<edges[1][2]) or not 0<=note[9]<=127 or note[10] not in (0,1<<note[0])):
            raise ValueError('invalid source, release or note fields')
        if not isinstance(completion,list) or len(completion)!=4 or any(type(x) is not int for x in completion):
            raise ValueError('invalid natural completion fields')
        by_voice[note[0]].append(note)
        if note[1]==5-held_voice:
            mask|=1<<index;end_half,end_frame,zero_half,zero_frame=completion
            if not note[3]<end_half<=zero_half<note[4] or not note[6]<end_frame<=zero_frame<note[7] or (
                    zero_frame-note[6]>768 or zero_frame-end_frame>32) or note[9]!=0 or note[10]!=1<<note[0]:
                raise ValueError('selected transient did not complete before gate release')
            natural=completion
        elif any(completion) or not note[9]:raise ValueError('loop completed naturally or became inactive before KOF')
    held,peer=by_voice[held_voice],by_voice[5-held_voice]
    if len(held)!=1 or len(peer)!=3 or held[0][1:3]!=[held_voice,PITCHES[2][9]] or (
            [n[1] for n in peer]!=[held_voice,5-held_voice,held_voice]) or any(n[2]!=PITCHES[2][12] for n in peer):
        raise ValueError('wrong loop/one-shot/loop source or pitch sequence')
    for first,second in zip(peer,peer[1:]):
        if not held[0][6]<=first[6]<first[7]<=first[8]<second[6]<held[0][7] or not first[3]<first[4]<=first[5]<second[3]:
            raise ValueError('instrument change overlaps release or interrupts held peer')
    if peer[-1][7]>held[0][7] or (not mask or mask & (mask-1)) or any(
            type(meta.get(k)) is not int or meta[k]!=mask for k in ('restore_ends','restore_natural_zeros')):
        raise ValueError('missing natural-completion restore coverage or held overlap')
    if any(a or b for a,b in pcm[edges[1][2]+3072:]):raise ValueError('final stop leaves stale audio')
    return dict(meta=meta,left=[p[0] for p in pcm],right=[p[1] for p in pcm],held=held[0],peer=peer,completion=natural)


def dual_pitch(values,target):
    """Fixed Hann periodogram resolves the owned 440/523-Hz pair.

    No gain fitting, PCM alignment or reference waveform is used. Scan a fixed
    +/-20-Hz band at 0.5 Hz and require both frequency and fundamental amplitude.
    The unchanged zero-crossing detector remains used for the isolated held tone.
    """
    if len(values)!=FRAMES:raise ValueError('dual-tone window length')
    weights=[0.5-0.5*math.cos(2*math.pi*i/(FRAMES-1)) for i in range(FRAMES)]
    total=sum(weights);mean=sum(v*w for v,w in zip(values,weights))/total
    samples=[(v-mean)*w for v,w in zip(values,weights)]
    frequencies=[target-20+0.5*i for i in range(81)]
    amplitudes=[]
    for frequency in frequencies:
        coefficient=2*math.cos(2*math.pi*frequency/RATE);previous=older=0.0
        for value in samples:
            current=value+coefficient*previous-older;older,previous=previous,current
        power=max(0.0,previous*previous+older*older-coefficient*previous*older)
        amplitudes.append(2*math.sqrt(power)/total)
    index=max(range(len(amplitudes)),key=amplitudes.__getitem__)
    frequency=frequencies[index];cents=1200*math.log2(frequency/target)
    if index in (0,80) or abs(cents)>10 or amplitudes[index]<100:
        raise ValueError('loop pair lacks an audible fundamental at the authored pitch')
    return dict(target_hz=target,frequency_hz=frequency,error_cents=cents,
                fundamental_peak_lsb=amplitudes[index],minimum_fundamental_peak_lsb=100,
                window_frames=FRAMES,scan_step_hz=0.5)


def compare(controls,model,held_voice):
    both=controls['both'];meta=both['meta']
    for profile in PROFILES[1:]:
        other=controls[profile]
        if any(meta[k]!=other['meta'][k] for k in ('frames','clocks','edges','notes','completions','gb_samples','native_samples')):
            raise ValueError('source controls do not share exact native timeline')
        if other['left']!=both['left']:raise ValueError('instrument changes disturb GB PCM')
    gb=controls['gb']
    if any(gb['right']):raise ValueError('silent source control is audible')
    maxima={}
    for channel in ('left','right'):
        maximum=max(abs(a-b-c+d) for a,b,c,d in zip(both[channel],controls['loop'][channel],controls['transient'][channel],gb[channel]))
        if maximum>SUM_TOLERANCE:raise ValueError('instrument-change sum differs from isolated sources')
        maxima[channel]=maximum
    loops=controls['loop'];transients=controls['transient']
    windows=[]
    for index,note in enumerate(both['peer']):
        begin=note[6]+32
        if begin+FRAMES>=note[7] or begin+FRAMES>=both['held'][7]:raise ValueError('pitch window crosses release')
        window=dict(selection=index,begin_frame=begin)
        if index==1:window['held']=pitch(loops['right'][begin:begin+FRAMES],440)
        else:
            window['held']=dual_pitch(loops['right'][begin:begin+FRAMES],440)
            window['selected_loop']=dual_pitch(loops['right'][begin:begin+FRAMES],440*2**(3/12))
        windows.append(window)
    _,transient,returned=both['peer'];completion=both['completion']
    begin=transient[6];stop=returned[6]-4;values=transients['right'][begin:stop]
    active=[i for i,v in enumerate(values) if v]
    if len(active)<32 or active[0]>128 or not 32<=active[-1]<=768 or max(abs(v) for v in values)<100:
        raise ValueError('selected one-shot is silent, stale or too long')
    quiet=completion[3]+32
    if stop-quiet<512 or any(transients['right'][quiet:stop]) or both['right'][quiet:stop]!=loops['right'][quiet:stop]:
        raise ValueError('one-shot completion leaves stale PCM or disturbs held loop')
    gaps=[]
    for a,b in zip(both['peer'],both['peer'][1:]):
        lo,hi=a[8]+4,b[6]-4
        if hi-lo<16 or any(transients['right'][lo:hi]) or both['right'][lo:hi]!=loops['right'][lo:hi] or max(abs(v) for v in loops['right'][lo:hi])<100:
            raise ValueError('instrument release gap disturbs peer or leaves stale output')
        gaps.append(dict(begin_frame=lo,end_frame=hi))
    # Require late sustained output too, so a brief attack cannot stand in for
    # the returned loop. The same pitch detector and limits apply at both ends.
    late=returned[7]-FRAMES-32
    if late<=returned[6]+32:raise ValueError('returned loop too short')
    returned_pitch=dual_pitch(loops['right'][late:late+FRAMES],440*2**(3/12))
    late_held=dual_pitch(loops['right'][late:late+FRAMES],440)
    begin=transient[6]+128;window=both['left'][begin:begin+FRAMES];midpoint=(max(window)+min(window))//2
    pulse=pitch([v-midpoint for v in window],(MASTER/5 if model=='sgb' else 4194304)/8192)
    return dict(maximum_sum_error_lsb=maxima,sum_tolerance_lsb=SUM_TOLERANCE,pitch_windows=windows,
                returned_late_pitch=returned_pitch,returned_late_held_pitch=late_held,independent_release_windows=gaps,gb_pulse=pulse,
                transient=dict(on_frame=transient[6],first_audible_frame=transient[6]+active[0],
                               last_audible_frame=transient[6]+active[-1],nonzero_frames=len(active),peak=max(abs(v) for v in values),
                               endx_frame=completion[1],natural_zero_frame=completion[3],gate_off_frame=transient[7],
                               quiet_begin=quiet,quiet_end=stop,pcm_sha256=digest(values)),
                matched_timeline=True,uninterrupted_gb_equal=True,final_silent=True)


def run(probe,host,directory,model,held_voice,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json'
    image=build(profile,held_voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(held_voice)],
                         capture_output=True,text=True,timeout=90)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded instrument-change probe failed: '+child.stderr[:256])
    if pcm.stat().st_size>600000 or report.stat().st_size>8192:raise ValueError('instrument-change output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());observed=observe(meta,raw,profile,held_voice)
    return observed,dict(model=model,held_voice=held_voice,profile=profile,mode=mode,
                         fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA image changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-instrument-change-audio-') as name:
        directory=Path(name);host=directory/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for held_voice in (2,3):
                controls={}
                for profile in PROFILES:
                    controls[profile],summary=run(probe,host,directory,model,held_voice,profile,'combined');runs.append(summary)
                comparisons.append(dict(model=model,held_voice=held_voice,**compare(controls,model,held_voice)))
                scalar,summary=run(probe,host,directory,model,held_voice,'both','scalar')
                if scalar!=controls['both']:raise ValueError('scalar instrument-change PCM/timeline differs')
                runs.append(summary)
    return dict(schema='gbb-sgb-instrument-change-audio-v1',qualification=False,playback=False,
                evidence='owned_48k_whole_host_instrument_change',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(report,indent=2))
