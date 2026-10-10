#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned rejection/mute PCM and suppressed SOUND while GB continues."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_bank_reject_audio_fixture import build,objects,FAULTS,PROFILES
from check_sgb_bank_replace_audio import program,OPTIONS,IMAGE_HASH,fnv,RATE,MASTER,FRAMES,pitch,digest


def observe(meta,raw,kind,profile,voice):
    expected=dict(schema='gbb-sgb-bank-replace-audio-v1',qualification=False,playback=False,
        reset_equal=True,restore_equal=True,sample_rate_hz=RATE,clipped=0,sounds=3,version=0xDA,
        active_upload=True,rejected_upload=kind,restore_edges=31,restore_releases=1,restore_zeros=1,
        queued_onsets=1,queued_offs=1,restore_banks=1,restore_clears=3,queued_banks=1,queued_clears=3,
        restore_mute=1,queued_mute=1,restore_rejects=7,queued_rejects=7)
    if kind not in FAULTS or profile not in PROFILES or type(voice) is not int or voice not in (2,3) or not isinstance(meta,dict) or any(
            type(meta.get(k)) is not type(v) or meta[k]!=v for k,v in expected.items()):
        raise ValueError('invalid rejection identity or replay coverage')
    bounds=dict(frames=(200000,250000),clocks=(100000000,100000100),restore_count=(1,1536),
        pending_restores=(1,1536),gb_samples=(1,250000),native_samples=(1,250000))
    if any(type(meta.get(k)) is not int or not lo<=meta[k]<=hi for k,(lo,hi) in bounds.items()) or (
            meta['pending_restores']>meta['restore_count'] or not isinstance(raw,bytes) or len(raw)!=meta['frames']*4 or
            meta['frames']!=meta['clocks']*RATE//MASTER):raise ValueError('invalid rejection output/counters')
    pcm=list(struct.iter_unpack('<hh',raw))
    if any(a in (-32768,32767) or b in (-32768,32767) for a,b in pcm):raise ValueError('hard-clipped rejection PCM')
    edges=meta.get('edges')
    if not isinstance(edges,list) or len(edges)!=5:raise ValueError('missing rejection command stage')
    previous=-1
    for stage,e in enumerate(edges,1):
        if not isinstance(e,list) or len(e)!=4 or any(type(x) is not int for x in e) or (
                e[:2]!=[stage,0 if stage==5 else 16] or not previous<e[2]<meta['frames'] or
                not 0<=e[3]*RATE//MASTER-e[2]<=2):raise ValueError('invalid rejection command frame/clock')
        previous=e[2]
    notes=meta.get('notes')
    if not isinstance(notes,list) or len(notes)!=1:raise ValueError('rejected bank started a new note')
    n=notes[0]
    if not isinstance(n,list) or len(n)!=11 or any(type(x) is not int for x in n) or (
            n[:3]!=[voice,2,1800] or not 0<n[3]<n[4]<=n[5] or
            not edges[0][2]<n[6]<edges[2][2]<=n[7]<=n[8]<edges[3][2] or
            n[7]-edges[2][2]>4096 or not 0<n[9]<=127 or n[10]!=1<<voice):
        raise ValueError('upload failed to interrupt an active loop')
    banks=meta.get('banks');clears=meta.get('clears');score,asset=objects(profile,voice)[0]
    if not isinstance(banks,list) or len(banks)!=1:raise ValueError('rejected bank was published')
    b=banks[0]
    if not isinstance(b,list) or len(b)!=7 or any(type(x) is not int for x in b) or (
            b[0]!=1 or b[3:]!=[fnv(score),fnv(asset),2,10] or
            not 0<b[1]<edges[0][2] or not 0<=b[2]*RATE//MASTER-b[1]<=2):raise ValueError('invalid old-bank publication')
    if not isinstance(clears,list) or len(clears)!=2:raise ValueError('rejection did not clear old RAM')
    for i,c in enumerate(clears):
        if not isinstance(c,list) or len(c)!=3 or any(type(x) is not int for x in c) or c[0]!=i or (
                not (n[8] if i else 0)<=c[1]<(edges[3][2] if i else b[1]) or
                not 0<=c[2]*RATE//MASTER-c[1]<=2):raise ValueError('invalid rejected-generation clearing')
    m=meta.get('mute')
    if not isinstance(m,list) or len(m)!=6 or any(type(x) is not int for x in m) or (
            not n[7]<=m[0]<=n[8] or not n[4]<=m[2]<=n[5] or m[3:5]!=[255,224] or
            not 0<=m[5]<=127 or not 0<=m[1]*RATE//MASTER-m[0]<=2):raise ValueError('missing upload KOF/mute boundary')
    gaps=meta.get('gaps')
    if not isinstance(gaps,list) or not 1<=len(gaps)<=64:raise ValueError('missing bounded transfer observations')
    frame=edges[2][2];sample=0
    for g in gaps:
        if not isinstance(g,list) or len(g)!=6 or any(type(x) is not int for x in g) or (
                not frame<g[0]<n[7] or g[4:]!=[3,0] or not sample<g[2]<g[3]<=meta['native_samples'] or
                not 2<=g[3]-g[2]<=512 or not 0<=g[1]*RATE//MASTER-g[0]<=2+(3*(g[3]-g[2])+1)//2):
            raise ValueError('sampling gap outside pre-KOF transfer interval')
        frame=g[0];sample=g[3]
    events=meta.get('rejects')
    if not isinstance(events,list) or len(events)!=3:raise ValueError('missing rejection or suppressed SOUND')
    if kind=='asset-gap':hashes=[fnv(bytes(2048)),fnv(bytes(192))]
    else:
        score,asset=objects('both',voice)[1];hashes=[fnv(bytes(2)+score[2:]),fnv(asset)]
    for i,e in enumerate(events):
        if not isinstance(e,list) or len(e)!=17 or any(type(x) is not int for x in e) or (
                e[:2]!=[1,i] or e[4:]!=[1,1 if kind=='asset-gap' else 2,1 if kind=='asset-gap' else 2,0xA4,0,0,0,0,0,224,255,*hashes] or
                not 0<=e[3]*RATE//MASTER-e[2]<=2):raise ValueError('rejection readiness, counters or bank contents are stale')
        lower=clears[1][1] if i==0 else edges[i+2][2]
        upper=edges[3][2] if i==0 else lower+960
        if not lower<=e[2]<upper or (i and not events[i-1][2]<e[2]):raise ValueError('suppression outside its SOUND interval')
    begin=n[8]+32
    if any(b for _,b in pcm[begin:]):raise ValueError('old or rejected bank sounds after mute')
    if profile=='gb' and any(b for _,b in pcm):raise ValueError('silent old-bank control is audible')
    if any(a or b for a,b in pcm[edges[4][2]+3072:]):raise ValueError('final GB route mute left a tail')
    return dict(meta=meta,left=[p[0] for p in pcm],right=[p[1] for p in pcm])


def compare(controls,model,voice):
    both,gb=(controls[p] for p in PROFILES);meta=both['meta']
    if any(meta[k]!=gb['meta'][k] for k in ('frames','clocks','edges','notes','clears','mute','gaps','rejects','gb_samples','native_samples')) or (
            meta['banks'][0][:3]+meta['banks'][0][5:]!=gb['meta']['banks'][0][:3]+gb['meta']['banks'][0][5:]):
        raise ValueError('silent control changes rejection timeline')
    if both['left']!=gb['left']:raise ValueError('rejection disturbs GB output')
    note=meta['notes'][0];windows=[]
    for begin in (note[6]+128,note[7]-FRAMES-32):
        if begin<note[6] or begin+FRAMES>=note[7]:raise ValueError('old tone window crosses interruption')
        windows.append(pitch(both['right'][begin:begin+FRAMES],440))
    pulses=[]
    for begin in (meta['rejects'][0][2]+128,meta['rejects'][1][2]+128):
        if begin+FRAMES>=meta['edges'][4][2]:raise ValueError('GB rejection window crosses final mute')
        values=both['left'][begin:begin+FRAMES];mid=(max(values)+min(values))//2
        pulses.append(pitch([x-mid for x in values],(MASTER/5 if model=='sgb' else 4194304)/8192))
    return dict(old_pitch_windows=windows,gb_pulse_windows=pulses,uninterrupted_gb_equal=True,
        rejected_bank_silent=True,blocked_sound_silent=True,final_silent=True,
        old_pcm_sha256=digest(both['right'][note[6]+128:note[6]+128+FRAMES]))


def run(probe,host,directory,model,voice,kind,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json';image=build(kind,profile,voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(voice),kind],
        capture_output=True,text=True,timeout=150)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded rejection probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>1000000 or report.stat().st_size>16384:raise ValueError('rejection output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());obs=observe(meta,raw,kind,profile,voice)
    return obs,dict(model=model,voice=voice,kind=kind,profile=profile,mode=mode,
        fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA firmware changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-bank-reject-audio-') as name:
        d=Path(name);host=d/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in FAULTS:
                    controls={}
                    for profile in PROFILES:
                        controls[profile],summary=run(probe,host,d,model,voice,kind,profile,'combined');runs.append(summary)
                    comparisons.append(dict(model=model,voice=voice,kind=kind,**compare(controls,model,voice)))
                    scalar,summary=run(probe,host,d,model,voice,kind,'both','scalar');runs.append(summary)
                    if scalar!=controls['both']:raise ValueError('scalar rejection PCM/timeline differs')
    return dict(schema='gbb-sgb-bank-reject-audio-v1',qualification=False,playback=False,
        evidence='owned_48k_blocked_rejected_bank',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:result=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))
