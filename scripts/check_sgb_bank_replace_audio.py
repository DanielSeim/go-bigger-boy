#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fresh owned PCM after physical uploaded-bank replacement with GB continuity."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_bank_replace_audio_fixture import build,objects,PROFILES
from build_sgb_score_transport import build as program
from check_sgb_host_sample_pitch import OPTIONS,IMAGE_HASH,fnv
from check_sgb_instrument_chromatic_reference import PITCHES
from check_sgb_polyphony_audio import SUM_TOLERANCE
from check_sgb_audio_transitions import RATE,MASTER,FRAMES,pitch,digest


def observe(meta,raw,profile,voice):
    expected=dict(schema='gbb-sgb-bank-replace-audio-v1',qualification=False,playback=False,
                  reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=4,version=0xDA,
                  restore_edges=31,restore_releases=3,restore_zeros=3,queued_onsets=3,queued_offs=3,
                  restore_banks=3,restore_clears=3,queued_banks=3,queued_clears=3)
    if profile not in PROFILES or type(voice) is not int or voice not in (2,3) or not isinstance(meta,dict) or any(
            type(meta.get(k)) is not type(v) or meta[k]!=v for k,v in expected.items()):
        raise ValueError('invalid bank-replacement identity or replay coverage')
    bounds=dict(frames=(200000,250000),clocks=(100000000,100000100),restore_count=(1,1536),
                pending_restores=(1,1536),gb_samples=(1,250000),native_samples=(1,250000))
    if any(type(meta.get(k)) is not int or not lo<=meta[k]<=hi for k,(lo,hi) in bounds.items()) or (
            meta['pending_restores']>meta['restore_count'] or not isinstance(raw,bytes) or len(raw)!=meta['frames']*4 or
            meta['frames']!=meta['clocks']*RATE//MASTER):raise ValueError('invalid output, counters or clock')
    pcm=list(struct.iter_unpack('<hh',raw))
    if any(a in (-32768,32767) or b in (-32768,32767) for a,b in pcm):raise ValueError('hard-clipped output')
    edges=meta.get('edges')
    if not isinstance(edges,list) or len(edges)!=5:raise ValueError('missing command stage')
    previous=-1
    for stage,edge in enumerate(edges,1):
        if not isinstance(edge,list) or len(edge)!=4 or any(type(x) is not int for x in edge) or (
                edge[:2]!=[stage,0 if stage==5 else 16]) or not previous<edge[2]<meta['frames'] or (
                not 0<=edge[3]*RATE//MASTER-edge[2]<=2):raise ValueError('invalid command frame/clock')
        previous=edge[2]
    notes=meta.get('notes')
    if not isinstance(notes,list) or len(notes)!=2:raise ValueError('expected old and new bank notes')
    for index,n in enumerate(notes):
        if not isinstance(n,list) or len(n)!=11 or any(type(x) is not int for x in n) or (
                n[:3]!=[voice,index+2,PITCHES[2][9 if index==0 else 12]] or not 0<n[3]<n[4]<=n[5] or
                not edges[0 if index==0 else 3][2]<n[6]<edges[1 if index==0 else 4][2]<=n[7]<=n[8]<meta['frames'] or
                n[7]-edges[1 if index==0 else 4][2]>960 or not 0<n[9]<=127 or n[10]!=1<<voice):
            raise ValueError('wrong source/pitch or stop did not release an active loop')
    if not notes[0][8]<edges[2][2] or not notes[0][5]<notes[1][3]:raise ValueError('old audio did not settle before upload')
    banks=meta.get('banks');clears=meta.get('clears');fixtures=objects(profile,voice)
    if not isinstance(banks,list) or len(banks)!=2 or not isinstance(clears,list) or len(clears)!=2:
        raise ValueError('missing atomic replacement timeline')
    for i,(bank,clear,(score,asset)) in enumerate(zip(banks,clears,fixtures)):
        if not isinstance(bank,list) or len(bank)!=7 or any(type(x) is not int for x in bank) or (
                bank[0]!=i+1 or bank[3:]!=[fnv(score),fnv(asset),*asset[24:26]]):raise ValueError('stale bank bytes or mapping')
        if not isinstance(clear,list) or len(clear)!=3 or any(type(x) is not int for x in clear) or clear[0]!=i:
            raise ValueError('invalid cleared generation')
        lower=edges[2][2] if i else -1;upper=edges[3 if i else 0][2]
        if not lower<clear[1]<=bank[1]<upper or not clear[2]<=bank[2] or any(
                not 0<=clock*RATE//MASTER-frame<=2 for frame,clock in ((clear[1],clear[2]),(bank[1],bank[2]))):
            raise ValueError('clearing/publication outside replacement interval')
    if any(a or b for a,b in pcm[edges[4][2]+3072:]):raise ValueError('final stop leaves stale audio')
    return dict(meta=meta,left=[x[0] for x in pcm],right=[x[1] for x in pcm])


def compare(controls,model,voice):
    both=controls['both'];meta=both['meta']
    for profile in PROFILES[1:]:
        other=controls[profile]
        if any(meta[k]!=other['meta'][k] for k in ('frames','clocks','edges','notes','clears','gb_samples','native_samples')) or (
                [b[:3]+b[5:] for b in meta['banks']]!=[b[:3]+b[5:] for b in other['meta']['banks']]):
            raise ValueError('source controls do not share exact replacement timeline')
        if both['left']!=other['left']:raise ValueError('replacement disturbs GB output')
    if any(controls['gb']['right']):raise ValueError('silent bank control is audible')
    maxima={}
    for channel in ('left','right'):
        maximum=max(abs(a-b-c+d) for a,b,c,d in zip(both[channel],controls['old'][channel],controls['new'][channel],controls['gb'][channel]))
        if maximum>SUM_TOLERANCE:raise ValueError('bank-replacement source sum exceeds budget')
        maxima[channel]=maximum
    old,new=meta['notes'];begin,end=old[8]+32,new[6]-4
    if end-begin<4096 or any(both['right'][begin:end]):raise ValueError('old bank leaves output during replacement gap')
    if any(controls['new']['right'][:new[6]]) or any(controls['old']['right'][begin:]):
        raise ValueError('source bank sounds outside its own lifetime')
    if both['right'][:begin]!=controls['old']['right'][:begin] or both['right'][new[6]:]!=controls['new']['right'][new[6]:]:
        raise ValueError('replacement retains stale old PCM or lacks fresh output')
    windows=[]
    for index,note in enumerate((old,new)):
        start=note[6]+128;late=note[7]-FRAMES-32;control=controls['old' if index==0 else 'new']
        if start+FRAMES>=note[7] or late<=start:raise ValueError('bank note too short for early/late pitch')
        target=440 if index==0 else 440*2**(3/12)
        windows.append(dict(bank=index+1,early=pitch(control['right'][start:start+FRAMES],target),
                            late=pitch(control['right'][late:late+FRAMES],target),
                            pcm_sha256=digest(control['right'][start:start+FRAMES])))
    if windows[0]['pcm_sha256']==windows[1]['pcm_sha256']:raise ValueError('replacement reuses old waveform')
    gb_begin=meta['clears'][1][1]+128
    if gb_begin+FRAMES>=meta['edges'][3][2]:raise ValueError('GB upload window crosses restart')
    values=both['left'][gb_begin:gb_begin+FRAMES];midpoint=(max(values)+min(values))//2
    pulse=pitch([v-midpoint for v in values],(MASTER/5 if model=='sgb' else 4194304)/8192)
    return dict(maximum_sum_error_lsb=maxima,sum_tolerance_lsb=SUM_TOLERANCE,pitch_windows=windows,gb_pulse=pulse,
                replacement_silent_begin=begin,replacement_silent_end=end,matched_timeline=True,
                fresh_bank_bytes=True,uninterrupted_gb_equal=True,final_silent=True)


def run(probe,host,directory,model,voice,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json'
    image=build(profile,voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(voice)],
                         capture_output=True,text=True,timeout=150)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded bank-replacement probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>1000000 or report.stat().st_size>16384:raise ValueError('replacement output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());observed=observe(meta,raw,profile,voice)
    return observed,dict(model=model,voice=voice,profile=profile,mode=mode,
                         fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA image changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-bank-replace-audio-') as name:
        directory=Path(name);host=directory/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                controls={}
                for profile in PROFILES:
                    controls[profile],summary=run(probe,host,directory,model,voice,profile,'combined');runs.append(summary)
                comparisons.append(dict(model=model,voice=voice,**compare(controls,model,voice)))
                scalar,summary=run(probe,host,directory,model,voice,'both','scalar')
                if scalar!=controls['both']:raise ValueError('scalar bank-replacement PCM/timeline differs')
                runs.append(summary)
    return dict(schema='gbb-sgb-bank-replace-audio-v1',qualification=False,playback=False,
                evidence='owned_48k_whole_host_stopped_bank_replacement',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(report,indent=2))
