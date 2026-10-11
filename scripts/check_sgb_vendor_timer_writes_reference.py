#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded public timer enable/target writes around authored reselection."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_tick_selection_fixture import build,TEMPO_POINTS
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_tick_selection_reference import summarize as phase_summary

ADDRESSES=(0xF1,0xFA,0xFB,0xFC)


def configuration(data):
    record=data.get('timer_configuration')
    if not isinstance(record,dict) or set(record)!={'end_half','boot_write_counts','requests','writes','final'}:
        raise ValueError('missing timer configuration fields')
    requests=data['requests'];end=record['end_half']
    if type(end) is not int or not requests[-1]<end<=2000000000:
        raise ValueError('invalid timer capture end')
    counts=record['boot_write_counts']
    if not isinstance(counts,list) or len(counts)!=4 or any(type(c) is not int or not 0<=c<=1000000 for c in counts):
        raise ValueError('invalid boot write counts')
    def state(values):
        if not isinstance(values,list) or len(values)!=4 or any(type(v) is not int or not -1<=v<=limit for v,limit in zip(values,(7,255,255,255))):
            raise ValueError('invalid timer configuration')
        return values
    snapshots=record['requests'];writes=record['writes']
    if not isinstance(snapshots,list) or len(snapshots)!=len(requests) or not isinstance(writes,list) or len(writes)>512:
        raise ValueError('invalid timer request/write bounds')
    previous=(-1,0)
    for write in writes:
        if (not isinstance(write,dict) or set(write)!={'half','request','address','value'} or
                any(type(v) is not int for v in write.values()) or write['address'] not in ADDRESSES or
                not 0<=write['value']<=(7 if write['address']==0xF1 else 255) or
                not 1<=write['request']<=len(requests)):
            raise ValueError('invalid timer write fields')
        request=write['request'];half=write['half']
        if not requests[request-1]<=half<=(requests[request] if request<len(requests) else end) or (half,request)<previous:
            raise ValueError('invalid timer write ownership/order')
        previous=(half,request)
    cache=None;stages=[]
    notes={}
    for event in data['events']:
        if event['register']==0x4C: notes.setdefault(event['request'],event['half'])
    for i,snapshot in enumerate(snapshots):
        if not isinstance(snapshot,dict) or set(snapshot)!={'request','initial'} or type(snapshot['request']) is not int or snapshot['request']!=i+1:
            raise ValueError('invalid timer initial snapshot')
        initial=state(snapshot['initial'])
        if cache is not None and initial!=cache or cache is None and any((v==-1)!=(n==0) for v,n in zip(initial,counts)):
            raise ValueError('timer snapshot does not follow writes')
        cache=initial.copy();note=notes[i+1]
        directory=max(r['half'] for r in data['selection_directory_reads'] if r['request']==i+1)
        root=min(r['half'] for r in data['selection_traversal_reads'] if r['request']==i+1 and
                 r['address']==0x2B10+4*i and r['half']>=directory)
        completion=data['timer_continuity'][i]['completion_half']
        item=dict(request=i+1,initial=initial,stages={name:dict(writes=0,enable_rises=0,target_changes=0,target_initializations=0) for name in ('dispatch','wait','setup','playback','idle')})
        for write in (w for w in writes if w['request']==i+1):
            half=write['half'];slot=ADDRESSES.index(write['address']);value=write['value'];old=cache[slot]
            stage='dispatch' if half<directory else 'wait' if half<root else 'setup' if half<note else 'idle' if completion and half>=completion else 'playback'
            stats=item['stages'][stage];stats['writes']+=1
            if slot==0 and old!=-1: stats['enable_rises']+=(value&~old).bit_count()
            if slot and old==-1: stats['target_initializations']+=1
            if slot and old!=-1 and old!=value: stats['target_changes']+=1
            cache[slot]=value
        item['final']=cache.copy();stages.append(item)
    if state(record['final'])!=cache:
        raise ValueError('final timer configuration does not follow writes')
    return dict(boot_write_counts=counts,requests=stages,writes=writes)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--tempo',type=int,choices=TEMPO_POINTS,action='append')
    args=parser.parse_args()
    try:
        tempos=tuple(dict.fromkeys(args.tempo or TEMPO_POINTS))
        with tempfile.TemporaryDirectory(prefix='gbb-timer-writes-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,tempo,state,holdout) for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                   for tempo in tempos for state in ('active','completed') for holdout in (False,True)]
            def run(case):
                model,name,tempo,state,holdout=case;image=build(tempo,state,holdout=holdout)
                game=root/f'{model}-{tempo}-{state}-{int(holdout)}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,
                             300000000,inputs,timer_writes=True)
                observed=phase_summary(data,tempo,state)
                return dict(model=model,tempo=tempo,state=state,holdout=holdout,
                            fixture_sha256=hashlib.sha256(image).hexdigest(),configuration=configuration(data),**observed)
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            report=dict(schema='gbb-sgb-vendor-timer-writes-v1',qualification=False,
                program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                for name in ('sgb1.program.rom','sgb2.program.rom')},runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded timer-write experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
