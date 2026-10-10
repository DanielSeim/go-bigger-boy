#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""First audible owned bank after cold preflight/semantic rejection."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from build_sgb_bank_cold_audio_fixture import build,objects,FAULTS,PROFILES
from check_sgb_bank_replace_audio import program,OPTIONS,IMAGE_HASH,RATE,MASTER,FRAMES,pitch,fnv,digest


def row(value,length):
    return isinstance(value,list) and len(value)==length and all(type(x) is int for x in value)


def observe(meta,raw,kind,profile,voice,*,semantic_tail=False,repeat=False):
    if any(type(x) is not bool for x in (semantic_tail,repeat)) or (semantic_tail and kind!='bad-root') or (repeat and not semantic_tail):
        raise ValueError('invalid cold tail case')
    retry=5 if repeat else 3
    stages=8 if repeat else 6
    failures=5 if repeat else 3
    clears_count=3 if repeat else 2
    generation=(1 if kind=='asset-gap' else 2)+(1 if repeat else 0)
    expected=dict(schema='gbb-sgb-bank-replace-audio-v1',qualification=False,playback=False,
        cold_rejection=True,recovery_upload=True,rejected_upload=kind,reset_equal=True,restore_equal=True,
        sample_rate_hz=RATE,clipped=0,sounds=5 if repeat else 4,version=0xDA,restore_edges=(1<<stages)-1,
        restore_releases=1,restore_zeros=1,queued_onsets=1,queued_offs=1,restore_banks=1,
        queued_banks=1,restore_clears=(1<<clears_count)-1,queued_clears=(1<<clears_count)-1,
        restore_rejects=(1<<failures)-1,queued_rejects=(1<<failures)-1,
        restore_recovered=1,queued_recovered=1)
    if kind not in FAULTS or profile not in PROFILES or type(voice) is not int or voice not in (2,3) or (
            not isinstance(meta,dict) or any(type(meta.get(k)) is not type(v) or meta[k]!=v for k,v in expected.items())):
        raise ValueError('invalid cold recovery identity or replay')
    if semantic_tail:
        tokens=meta.get('consumed_tokens')
        if meta.get('semantic_tail') is not True or type(meta.get('repeated_rejection')) is not bool or meta['repeated_rejection']!=repeat or (
                not row(tokens,failures) or any(t!=3 for t in tokens)):
            raise ValueError('missing cold one-byte consumed-token synchronization')
    bounds=dict(frames=(200000,250000),clocks=(100000000,100000100),restore_count=(1,1536),
                pending_restores=(1,1536),gb_samples=(1,250000),native_samples=(1,250000))
    if any(type(meta.get(k)) is not int or not lo<=meta[k]<=hi for k,(lo,hi) in bounds.items()) or (
            meta['pending_restores']>meta['restore_count'] or not isinstance(raw,bytes) or len(raw)!=meta['frames']*4 or
            meta['frames']!=meta['clocks']*RATE//MASTER):raise ValueError('invalid cold recovery counters/output')
    pcm=list(struct.iter_unpack('<hh',raw))
    if any(a in (-32768,32767) or b in (-32768,32767) for a,b in pcm):raise ValueError('clipped cold recovery PCM')
    def timed(frame,clock):return 0<=frame<meta['frames'] and 0<=clock*RATE//MASTER-frame<=2
    edges=meta.get('edges')
    if not isinstance(edges,list) or len(edges)!=stages:raise ValueError('missing cold command stages')
    previous=-1
    for stage,e in enumerate(edges,1):
        if not row(e,4) or e[:2]!=[stage,0 if stage==stages or profile=='native' else 16] or (
                not previous<e[2] or not timed(e[2],e[3])):raise ValueError('invalid cold command timeline')
        previous=e[2]
    notes=meta.get('notes')
    if not isinstance(notes,list) or len(notes)!=1:raise ValueError('cold rejection started a note')
    n=notes[0]
    if not row(n,11) or n[:3]!=[voice,3,2140] or not 0<n[3]<n[4]<=n[5] or (
            not edges[retry+1][2]<n[6]<edges[retry+2][2]<=n[7]<=n[8]<meta['frames'] or
            n[7]-edges[retry+2][2]>960 or not 0<n[9]<=127 or n[10]!=1<<voice):raise ValueError('invalid first admitted note/release')
    error=1 if kind=='asset-gap' else 2
    score,asset=objects('gb' if profile=='gb' else 'both',voice)[1];fresh=[fnv(score),fnv(asset)]
    banks=meta.get('banks');clears=meta.get('clears');events=meta.get('rejects');adopt=meta.get('recovered')
    if not isinstance(banks,list) or len(banks)!=1:raise ValueError('cold failure published a bank')
    b=banks[0]
    if not row(b,7) or b[0]!=generation or b[3:]!=[*fresh,10,2] or (
            not edges[retry][2]<b[1]<edges[retry+1][2] or not timed(b[1],b[2])):raise ValueError('invalid first bank publication')
    if not isinstance(clears,list) or len(clears)!=clears_count or any(not row(c,3) for c in clears):raise ValueError('missing cold clear')
    clear_stages=[None,3,retry] if repeat else [None,retry]
    for i,c in enumerate(clears):
        lower=0 if i==0 else edges[clear_stages[i]][2]
        upper=edges[0][2] if i==0 else edges[4][2] if repeat and i==1 else b[1]
        if c[0]!=(error+i-2 if i else 0) or not lower<c[1]<upper or not timed(c[1],c[2]):
            raise ValueError('invalid cold clear generation/timing')
    if not isinstance(events,list) or len(events)!=failures:raise ValueError('missing cold failure/suppression')
    score,asset=objects('both',voice)[1]
    failed=[fnv(bytes(2048)),fnv(bytes(192))] if kind=='asset-gap' else [fnv(bytes(2)+score[2:]),fnv(asset)]
    for i,e in enumerate(events):
        second=repeat and i>=3
        rejection=i in (0,3)
        lower=clears[0 if i==0 else 1][1] if rejection else edges[i][2]
        upper=edges[0 if i==0 else 4][2] if rejection else min(edges[i+1][2],lower+960)
        if not row(e,17) or e[:2]!=([2,2 if i==3 else 3] if second else [1,i]) or (
                e[4:]!=[1,error,error-1+int(second),164,0,0,0,0,0,224,255,*failed] or
                not lower<=e[2]<upper or not timed(e[2],e[3]) or (i and not events[i-1][2]<e[2])):
            raise ValueError('stale cold error, readiness, RAM or suppression')
    if not row(adopt,18) or adopt[2:]!=[generation,2 if repeat else 1,3 if repeat else 2,0,1,0,165,0,1,0,0,3,224,255,*fresh] or (
            not b[1]<=adopt[0]<edges[retry+1][2] or not timed(adopt[0],adopt[1])):raise ValueError('cold bank not admitted before first SOUND')
    if any(b for _,b in pcm[:n[6]]):raise ValueError('native PCM before first admitted note')
    if profile=='gb' and any(b for _,b in pcm):raise ValueError('silent bank control is audible')
    if profile=='native' and any(a for a,_ in pcm):raise ValueError('native-only control leaks GB audio')
    if any(a for a,_ in pcm[:max(0,edges[0][2]-4)]):raise ValueError('GB sounds before its first command')
    if any(a or b for a,b in pcm[edges[-1][2]+3072:]):raise ValueError('cold recovery stop leaves a tail')
    return dict(meta=meta,left=[a for a,_ in pcm],right=[b for _,b in pcm])


def compare(controls,model,voice):
    both,native,gb=(controls[p] for p in PROFILES);m=both['meta'];note=m['notes'][0]
    for other in (native,gb):
        q=other['meta']
        if any(m[k]!=q[k] for k in ('frames','clocks','notes','clears','rejects','gb_samples','native_samples')) or (
                [e[:1]+e[2:] for e in m['edges']]!=[e[:1]+e[2:] for e in q['edges']] or
                [b[:3]+b[5:] for b in m['banks']]!=[b[:3]+b[5:] for b in q['banks']] or
                m['recovered'][:16]!=q['recovered'][:16]):raise ValueError('cold source controls change native timeline')
    if any(native['left']) or any(gb['right']) or both['left']!=gb['left'] or both['right']!=native['right']:
        raise ValueError('cold source isolation differs')
    maxima={channel:max(abs(a-b-c) for a,b,c in zip(both[channel],native[channel],gb[channel])) for channel in ('left','right')}
    if any(v>4 for v in maxima.values()):raise ValueError('cold source sum exceeds budget')
    start=note[6]+128;late=note[7]-FRAMES-32
    if start+FRAMES>=note[7] or late<=start:raise ValueError('first note too brief')
    tones=[pitch(native['right'][begin:begin+FRAMES],440*2**(3/12)) for begin in (start,late)]
    pulses=[]
    retry=5 if len(m['edges'])==8 else 3
    windows=[(m['edges'][0][2]+128,m['edges'][1][2]),
                        (m['rejects'][1][2]+128,m['edges'][2][2]),
                        (m['rejects'][2][2]+128,m['edges'][3][2]),
                        (m['clears'][-1][1]+128,m['edges'][retry+1][2]),
                        (note[6]+128,m['edges'][retry+2][2])]
    if retry==5:
        windows[3:3]=[(m['clears'][1][1]+128,m['edges'][4][2]),
                      (m['rejects'][4][2]+128,m['edges'][5][2])]
    for begin,limit in windows:
        if begin+FRAMES>=limit:raise ValueError('cold GB window crosses command')
        values=gb['left'][begin:begin+FRAMES];mid=(max(values)+min(values))//2
        pulses.append(pitch([x-mid for x in values],(MASTER/5 if model=='sgb' else 4194304)/8192))
    return dict(maximum_sum_error_lsb=maxima,sum_tolerance_lsb=4,pitch_windows=tones,gb_pulse_windows=pulses,
                native_silent_before=note[6],fresh_pcm_sha256=digest(native['right'][start:start+FRAMES]),
                first_bank_admitted=True,fresh_restart_audible=True,matched_timeline=True,final_silent=True)


def run(probe,host,directory,model,voice,kind,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json';image=build(kind,profile,voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(voice),kind,'cold'],capture_output=True,text=True,timeout=150)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded cold probe failed: '+child.stderr[:512])
    if pcm.stat().st_size>1000000 or report.stat().st_size>16384:raise ValueError('cold output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());obs=observe(meta,raw,kind,profile,voice)
    return obs,dict(model=model,voice=voice,kind=kind,profile=profile,mode=mode,
                    fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe,kinds=FAULTS):
    if not isinstance(kinds,tuple) or not kinds or len(set(kinds))!=len(kinds) or any(k not in FAULTS for k in kinds):raise ValueError('requires distinct cold failures')
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA firmware changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-bank-cold-audio-') as name:
        d=Path(name);host=d/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for voice in (2,3):
                for kind in kinds:
                    controls={}
                    for profile in PROFILES:
                        controls[profile],summary=run(probe,host,d,model,voice,kind,profile,'combined');runs.append(summary)
                    comparisons.append(dict(model=model,voice=voice,kind=kind,**compare(controls,model,voice)))
                    scalar,summary=run(probe,host,d,model,voice,kind,'both','scalar');runs.append(summary)
                    if scalar!=controls['both']:raise ValueError('scalar cold PCM/timeline differs')
    return dict(schema='gbb-sgb-bank-cold-audio-v1',qualification=False,playback=False,
                evidence='owned_48k_cold_rejection_first_bank',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--kind',choices=FAULTS,help='run one complete failure matrix; default runs both')
    args=parser.parse_args()
    try:result=measure(args.probe.resolve(),(args.kind,) if args.kind else FAULTS)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))
