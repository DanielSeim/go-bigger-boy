#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned 48-kHz SNES polyphony, independent release and retrigger evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_polyphony_audio_fixture import build,PROFILES,PEER_NOTES
from build_sgb_score_transport import build as program
from check_sgb_host_sample_pitch import OPTIONS,IMAGE_HASH
from check_sgb_instrument_chromatic_reference import PITCHES
from check_sgb_audio_transitions import RATE,MASTER,FRAMES,pitch,digest

SUM_TOLERANCE=4


def observe(meta,raw,profile,held_voice):
    expected=dict(schema='gbb-sgb-polyphony-audio-v1',qualification=False,playback=False,
                  reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=2,version=0xDA,
                  restore_edges=3,restore_releases=31,restore_zeros=31)
    if profile not in PROFILES or type(held_voice) is not int or held_voice not in (2,3) or not isinstance(meta,dict) or any(
            type(meta.get(k)) is not type(v) or meta[k]!=v for k,v in expected.items()):
        raise ValueError('invalid polyphony identity or lifecycle coverage')
    bounds=dict(frames=(100000,150000),clocks=(55000000,55000100),restore_count=(1,1024),
                pending_restores=(1,1024),gb_samples=(1,150000),native_samples=(1,150000))
    if any(type(meta.get(k)) is not int or not lo<=meta[k]<=hi for k,(lo,hi) in bounds.items()) or (
            meta['pending_restores']>meta['restore_count'] or not isinstance(raw,bytes) or len(raw)!=meta['frames']*4 or
            meta['frames']!=meta['clocks']*RATE//MASTER):
        raise ValueError('invalid polyphony PCM, counters or clock')
    pcm=list(struct.iter_unpack('<hh',raw))
    if any(a in (-32768,32767) or b in (-32768,32767) for a,b in pcm):
        raise ValueError('hard-clipped polyphony output')
    edges=meta.get('edges')
    if not isinstance(edges,list) or len(edges)!=2:
        raise ValueError('missing lifecycle edge')
    previous=-1
    for stage,edge in enumerate(edges,1):
        if not isinstance(edge,list) or len(edge)!=4 or any(type(x) is not int for x in edge) or (
                edge[:2]!=[stage,16 if stage==1 else 0]) or not previous<edge[2]<meta['frames'] or (
                not 0<=edge[3]*RATE//MASTER-edge[2]<=2):
            raise ValueError('invalid lifecycle frame/clock')
        previous=edge[2]
    notes=meta.get('notes')
    if not isinstance(notes,list) or len(notes)!=5:
        raise ValueError('expected one held note and four peer notes')
    by_voice={2:[],3:[]}
    for note in notes:
        if not isinstance(note,list) or len(note)!=9 or any(type(x) is not int for x in note) or (
                note[0] not in by_voice) or note[1]!=note[0] or not 0<note[3]<note[4]<=note[5] or (
                not edges[0][2]<note[6]<note[7]<=note[8]<edges[1][2]):
            raise ValueError('wrong source or incomplete independent release')
        by_voice[note[0]].append(note)
    held,peer=by_voice[held_voice],by_voice[5-held_voice]
    if len(held)!=1 or len(peer)!=4 or held[0][2]!=PITCHES[2][9] or (
            [n[2] for n in peer]!=[PITCHES[2][n-24] for n in PEER_NOTES]):
        raise ValueError('wrong held/peer pitch sequence')
    for first,second in zip(peer,peer[1:]):
        if not held[0][6]<=first[6]<first[7]<=first[8]<second[6]<held[0][7]:
            raise ValueError('peer retrigger does not independently release inside held note')
    if any(left or right for left,right in pcm[edges[1][2]+3072:]):
        raise ValueError('polyphony final stop leaves stale audio')
    return dict(meta=meta,left=[x[0] for x in pcm],right=[x[1] for x in pcm],held=held[0],peer=peer)


def compare(controls,model,held_voice):
    both=controls['both']; meta=both['meta']
    for profile in PROFILES[1:]:
        other=controls[profile]['meta']
        if any(meta[k]!=other[k] for k in ('frames','clocks','edges','notes','gb_samples','native_samples')):
            raise ValueError('source controls do not share KON/release/output timeline')
    if any(controls[p]['left']!=both['left'] for p in PROFILES[1:]):
        raise ValueError('SNES controls disturb uninterrupted GB PCM')
    gb=controls['gb']
    if any(gb['right']):
        raise ValueError('left-panned SNES controls leak audio into right channel')
    maxima=[]
    for channel in ('left','right'):
        residual=[a-b-c+d for a,b,c,d in zip(both[channel],controls['voice2'][channel],controls['voice3'][channel],gb[channel])]
        maximum=max(abs(x) for x in residual)
        if maximum>SUM_TOLERANCE:
            raise ValueError('polyphony sum differs from matched sources')
        maxima.append(maximum)
    held=controls[f'voice{held_voice}']; peer=controls[f'voice{5-held_voice}']
    measurements=[]
    for index,note in enumerate(both['peer']):
        begin=note[6]+32
        if begin+FRAMES>=note[7] or begin+FRAMES>=both['held'][7]:
            raise ValueError('pitch window crosses independent release')
        peer_target=440*2**((PEER_NOTES[index]-33)/12)
        measurements.append(dict(peer_note=PEER_NOTES[index],begin_frame=begin,
                                 peer=pitch(peer['right'][begin:begin+FRAMES],peer_target),
                                 held=pitch(held['right'][begin:begin+FRAMES],440)))
    quiet=[]
    for first,second in zip(both['peer'],both['peer'][1:]):
        begin,end=first[8]+4,second[6]-4
        if end-begin<16 or any(peer['right'][begin:end]) or both['right'][begin:end]!=held['right'][begin:end] or (
                max(abs(x) for x in held['right'][begin:end])<100):
            raise ValueError('peer release silences held voice or leaves stale peer PCM')
        quiet.append(dict(begin_frame=begin,end_frame=end,held_pcm_sha256=digest(held['right'][begin:end])))
    begin=both['held'][6]+128
    gb_window=both['left'][begin:begin+FRAMES]
    midpoint=(max(gb_window)+min(gb_window))//2
    pulse=pitch([x-midpoint for x in gb_window],(MASTER/5 if model=='sgb' else 4194304)/8192)
    return dict(maximum_sum_error_lsb=dict(zip(('left','right'),maxima)),sum_tolerance_lsb=SUM_TOLERANCE,
                pitch_windows=measurements,independent_release_windows=quiet,gb_pulse=pulse,
                matched_timeline=True,uninterrupted_gb_equal=True,final_silent=True)


def run(probe,host,directory,model,held_voice,profile,mode):
    game=directory/'game.gb'; pcm=directory/'output.pcm'; report=directory/'output.json'
    image=build(profile,held_voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report)],
                         capture_output=True,text=True,timeout=90)
    if child.returncode or child.stdout or child.stderr:
        raise ValueError('bounded polyphony probe failed: '+child.stderr[:256])
    if pcm.stat().st_size>600000 or report.stat().st_size>8192:
        raise ValueError('polyphony output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text())
    observed=observe(meta,raw,profile,held_voice)
    return observed,dict(model=model,held_voice=held_voice,profile=profile,mode=mode,
                         fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),
                         left_pcm_sha256=digest(observed['left']),right_pcm_sha256=digest(observed['right']),**meta)


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA image changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-polyphony-audio-') as name:
        directory=Path(name);host=directory/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for held_voice in (2,3):
                controls={}
                for profile in PROFILES:
                    controls[profile],summary=run(probe,host,directory,model,held_voice,profile,'combined')
                    runs.append(summary)
                comparisons.append(dict(model=model,held_voice=held_voice,**compare(controls,model,held_voice)))
                scalar,summary=run(probe,host,directory,model,held_voice,'both','scalar')
                if scalar!=controls['both']:raise ValueError('scalar polyphony PCM/timeline differs')
                runs.append(summary)
    return dict(schema='gbb-sgb-polyphony-audio-v1',qualification=False,playback=False,
                evidence='owned_48k_whole_host_polyphony',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(report,indent=2))
