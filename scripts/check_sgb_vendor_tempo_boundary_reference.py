#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe pending old-track playback around an authored E7 and reselection."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_tick_selection_fixture import build
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_timer_tempo_reference import continuity
from check_sgb_vendor_timer_writes_reference import configuration
from check_sgb_vendor_selection_phase_reference import timer_summary
from check_sgb_vendor_tick_reference import expected
from check_sgb_vendor_selection_wait_reference import WAITS

GAPS={'sgb':(8,9,10,11),'sgb2':(4,5,6,7)}
SPINS=(0,1024)


def summarize(data,live):
    if type(live) is not int or live not in (32,128) or data['music']!=[1,2] or len(data['requests'])!=2:
        raise ValueError('invalid boundary request controls')
    notes=[e for e in data['events'] if e['register']==0x4C]
    if not 2<=len(notes)<=6 or notes[-1]['pitch']!=717 or notes[-1]['request']!=2:
        raise ValueError('missing distinct selected note')
    old=notes[:-1];selected=notes[-1];request=data['requests'][1]
    if ([n['pitch'] for n in old]!=[1437,1357,1437,1357,1281][:len(old)] or
            old[0]['request']!=1 or not old[0]['half']<request<=selected['half'] or
            any(n['srcn']!=2 or n['adsr1']!=255 or n['adsr2']!=224 or n['gain']!=184 for n in notes)):
        raise ValueError('boundary note sequence or binding changed')
    if any(e['register']==0x5C and e['value']==255 and old[0]['half']<e['half']<request for e in data['events']):
        raise ValueError('old song completed before request')
    for note,next_note in zip(old[:4],old[1:5]):
        gate=next((e['half'] for e in data['events'] if e['register']==0x5C and e['value']==4 and note['half']<e['half']<next_note['half']),None)
        if gate is None or not any(e['register']==0x5C and e['value']==0 and gate<e['half']<next_note['half'] for e in data['events']):
            raise ValueError('old-track gate or clear missing')
    if len(old)>1:
        interval=old[1]['half']-old[0]['half']
        if not min(expected(128,p)[0] for p in range(256))-8192<=interval<=max(expected(128,p)[0] for p in range(256))+8192:
            raise ValueError('starting tempo witness changed')
    if len(old)>=4:
        observed=[old[i]['half']-old[1]['half'] for i in (2,3)]
        if min(max(abs(a-b) for a,b in zip(expected(128,p)[:2],observed)) for p in range(256))>4096:
            raise ValueError('starting prefix cadence changed')
    # Delivery counters remain untouched on every event. Only the distinct
    # selected pitch starts request 2's setup/continuity window in this mode.
    collapsed={**data,'events':[e for e in data['events'] if e['register']!=0x4C or e is old[0] or e is selected]}
    windows=continuity(collapsed)['windows']
    if windows[0]['completion_half'] or not windows[1]['completion_half']:
        raise ValueError('boundary completion state changed')
    selections=[];anchors=[]
    for i,note in enumerate((old[0],selected)):
        reads=[r for r in data['selection_directory_reads'] if r['request']==i+1]
        if ({r['address'] for r in reads}!={0x2B00+2*i,0x2B01+2*i} or
                any(not data['requests'][i]<=r['half']<note['half'] for r in reads)):
            raise ValueError('boundary directory ownership changed')
        directory=max(r['half'] for r in reads);previous=directory;root=None
        for low,high,required in ((0x2B10+4*i,0x2B13+4*i,{0x2B10+4*i,0x2B11+4*i}),
                (0x2B20+16*i,0x2B2F+16*i,{0x2B24+16*i,0x2B25+16*i}),
                (0x2B60+48*i,0x2B6C+48*i,{0x2B60+48*i,0x2B6C+48*i})):
            live_reads=[r for r in data['selection_traversal_reads'] if r['request']==i+1 and
                        low<=r['address']<=high and previous<=r['half']<note['half']]
            if not required<={r['address'] for r in live_reads}:
                raise ValueError('boundary traversal incomplete')
            if root is None: root=min(r['half'] for r in live_reads)
            previous=max(r['half'] for r in live_reads)
        anchors.append((directory,root))
        selections.append(dict(traversal_stages=dict(root=dict(after_previous_stage_ms=(root-directory)/2048))))
    timer_summary(collapsed,selections)
    config=configuration(collapsed);reads=data.get('tempo_operand_reads')
    if not isinstance(reads,list) or len(reads)>64:
        raise ValueError('invalid boundary operand bounds')
    previous=(-1,0)
    for read in reads:
        if (not isinstance(read,dict) or set(read)!={'half','request','address'} or
                any(type(v) is not int for v in read.values()) or read['address']!=0x2B70 or read['request'] not in (1,2)):
            raise ValueError('invalid boundary operand fields')
        half=read['half'];owner=read['request']
        if (not max(old[0]['half'],data['requests'][owner-1])<=half<=(request if owner==1 else data['timer_configuration']['end_half']) or
                (half,owner)<previous):
            raise ValueError('invalid boundary operand ownership/order')
        previous=(half,owner)
    # Multiple reads may include owned prevalidation. Do not equate those with
    # a uniquely observed command consumption or choose one to fit an outcome.
    if not reads and len(old)>=4: raise ValueError('missing consumed operand observation')
    age=None;category='not_observed' if not reads else 'ambiguous_reads'
    if len(reads)==1:
        half=reads[0]['half']
        if len(old)<3 or not old[2]['half']<half<(old[3]['half'] if len(old)>=4 else selected['half']):
            raise ValueError('operand not bracketed by old-track notes')
        age=(request-half)/2048
        category='operand_before_request' if reads[0]['request']==1 else 'request_before_operand'
    observed=(anchors[1][1]-anchors[1][0])/2048
    return dict(category=category,operand_age_ms=age,operand_reads=reads,
        operand_before_directory=reads[0]['half']<anchors[1][0] if len(reads)==1 else None,
        near_boundary=age is not None and abs(age)<=4,
        old_note_requests=[n['request'] for n in old],old_note_count=len(old),
        selected_half=selected['half'],request_half=request,directory_half=anchors[1][0],root_half=anchors[1][1],
        wait_ms=observed,starting_error_ms=abs(observed-WAITS[128]),live_error_ms=abs(observed-WAITS[live]),
        starting_within_2ms=abs(observed-WAITS[128])<=2,live_within_2ms=abs(observed-WAITS[live])<=2,
        configuration=config)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-tempo-boundary-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            cases=[(model,name,live,gap,spin) for model,name in (('sgb','sgb1.program.rom'),('sgb2','sgb2.program.rom'))
                   for live in (32,128) for gap in GAPS[model] for spin in SPINS]
            def run(case):
                model,name,live,gap,spin=case
                image=build(128,live_tempo=live,change_after=3,boundary=True,request_gap=gap,selection_spin=spin)
                game=root/f'{model}-{live}-{gap}-{spin}.gb';game.write_bytes(image)
                data=capture(args.probe.resolve(),(args.firmware_dir/name).resolve(),game,model,300000000,inputs,tempo_boundary=True)
                return dict(model=model,live=live,gap=gap,spin=spin,fixture_sha256=hashlib.sha256(image).hexdigest(),**summarize(data,live))
            with ThreadPoolExecutor(max_workers=4) as pool: runs=list(pool.map(run,cases))
            report=dict(schema='gbb-sgb-vendor-tempo-boundary-v1',qualification=False,parameters_fitted=False,
                program_sha256={name:hashlib.sha256((args.firmware_dir/name).read_bytes()).hexdigest()
                                for name in ('sgb1.program.rom','sgb2.program.rom')},runs=runs)
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded tempo-boundary experiment failed; child diagnostics suppressed\n')


if __name__=='__main__': main()
