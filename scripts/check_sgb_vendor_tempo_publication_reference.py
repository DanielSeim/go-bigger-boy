#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Advance authored reselection and retain startup overlap as a distinct outcome."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_tick_selection_fixture import build
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_tempo_boundary_reference import summarize as boundary_summary, operand_summary
from check_sgb_vendor_tempo_handoff_reference import handoff
from check_sgb_vendor_timer_tempo_reference import continuity
from check_sgb_vendor_selection_phase_reference import timer_summary
from check_sgb_vendor_selection_wait_reference import WAITS
from check_sgb_vendor_tick_reference import expected

GAPS={'sgb':(7,8),'sgb2':(3,4)}
SPINS=(0,256,512,768,1024,1536,2048)


def cases():
    return [(m,l,g,s) for m in GAPS for l in (32,128) for g in GAPS[m] for s in SPINS]


def startup_summary(data,live):
    if type(live) is not int or live not in (32,128) or data['music']!=[1,2] or len(data['requests'])!=2:
        raise ValueError('invalid startup request controls')
    notes=[e for e in data['events'] if e['register']==0x4C]
    if not 2<=len(notes)<=4 or [n['pitch'] for n in notes[:-1]]!=[1437,1357,1437][:len(notes)-1] or notes[-1]['pitch']!=717:
        raise ValueError('invalid interrupted startup prefix')
    old=notes[:-1];selected=notes[-1];request=data['requests'][1]
    if (not request<=old[0]['half']<selected['half'] or
            any(n['request']!=2 or [n[k] for k in ('srcn','adsr1','adsr2','gain')]!=[2,255,224,184] for n in notes)):
        raise ValueError('invalid startup note ownership/binding')
    if len(old)>1:
        interval=old[1]['half']-old[0]['half']
        if not min(expected(128,p)[0] for p in range(256))-8192<=interval<=max(expected(128,p)[0] for p in range(256))+8192:
            raise ValueError('startup prefix cadence changed')
    for key,bound in (('selection_directory_reads',128),('selection_traversal_reads',512)):
        reads=data[key]
        if not isinstance(reads,list) or not 1<=len(reads)<=bound:raise ValueError('invalid startup read bounds')
        previous=(-1,0)
        for read in reads:
            if (not isinstance(read,dict) or set(read)!={'half','request','address'} or
                    any(type(v) is not int for v in read.values()) or read['request'] not in (1,2) or
                    not 0<=read['address']<=65535 or (read['half'],read['request'])<previous or
                    not data['requests'][read['request']-1]<=read['half']<=
                        (request if read['request']==1 else data['timer_configuration']['end_half'])):
                raise ValueError('invalid startup read ownership/fields')
            previous=(read['half'],read['request'])
    for note,next_note in zip(old,old[1:]):
        gate=next((e['half'] for e in data['events'] if e['register']==0x5C and e['value']==4 and note['half']<e['half']<next_note['half']),None)
        if gate is None or not any(e['register']==0x5C and e['value']==0 and gate<e['half']<next_note['half'] for e in data['events']):
            raise ValueError('missing interrupted startup gate/clear')
    release=next((e['half'] for e in data['events'] if e['register']==0x5C and e['value']==255 and old[-1]['half']<e['half']<selected['half']),None)
    if release is None or not any(e['register']==0x5C and e['value']==0 and release<e['half']<selected['half'] for e in data['events']):
        raise ValueError('missing startup selection release/clear')
    anchors=[]
    for i,note in enumerate((old[0],selected)):
        reads=[r for r in data['selection_directory_reads'] if r['address'] in (0x2B00+2*i,0x2B01+2*i)]
        if ({r['address'] for r in reads}!={0x2B00+2*i,0x2B01+2*i} or
                any(r['request']!=i+1 or not data['requests'][i]<=r['half']<note['half'] for r in reads)):
            raise ValueError('invalid startup directory ownership')
        directory=max(r['half'] for r in reads);previous=directory;root=None
        for low,high,required in ((0x2B10+4*i,0x2B13+4*i,{0x2B10+4*i,0x2B11+4*i}),
                (0x2B20+16*i,0x2B2F+16*i,{0x2B24+16*i,0x2B25+16*i}),
                (0x2B60+48*i,0x2B6C+48*i,{0x2B60+48*i,0x2B6C+48*i})):
            reads=[r for r in data['selection_traversal_reads'] if low<=r['address']<=high and previous<=r['half']<note['half']]
            if not required<={r['address'] for r in reads} or any(r['request']!=2 or r['half']<request for r in reads):
                raise ValueError('invalid startup traversal ownership')
            if root is None:root=min(r['half'] for r in reads)
            previous=max(r['half'] for r in reads)
        anchors.append((directory,root))
    # Startup crossed delivery 2 before the first KON. Do not invent a request-1
    # continuity or timer window, or relabel old-note delivery counters.
    collapsed={**data,'events':[e for e in data['events'] if e['register']!=0x4C or e is selected]}
    windows=continuity(collapsed)['windows']
    if not windows[0]['completion_half']:raise ValueError('selected song did not complete')
    directory,root=anchors[1]
    timer_summary(collapsed,[dict(traversal_stages=dict(root=dict(after_previous_stage_ms=(root-directory)/2048)))])
    config=data['timer_configuration']
    if (not isinstance(config,dict) or set(config)!={'end_half','boot_write_counts','requests','writes','final'} or
            type(config['end_half']) is not int or not selected['half']<config['end_half']<=2000000000 or
            config['end_half']!=windows[0]['end_half'] or
            config['writes']!=[] or not isinstance(config['boot_write_counts'],list) or len(config['boot_write_counts'])!=4 or
            any(type(v) is not int or not 0<=v<=1000000 for v in config['boot_write_counts']) or
            not isinstance(config['requests'],list) or len(config['requests'])!=2):
        raise ValueError('startup requires bounded stable timer configuration')
    initial=config['final']
    if (not isinstance(initial,list) or len(initial)!=4 or
            any(type(v) is not int or not -1<=v<=limit or (v==-1)!=(count==0)
                for v,limit,count in zip(initial,(7,255,255,255),config['boot_write_counts'])) or
            any(not isinstance(r,dict) or set(r)!={'request','initial'} or type(r['request']) is not int or
                r['request']!=i+1 or r['initial']!=initial or not isinstance(r['initial'],list) or
                any(type(v) is not int for v in r['initial']) for i,r in enumerate(config['requests']))):
        raise ValueError('startup timer snapshots do not agree')
    observed=(root-directory)/2048
    return dict(**operand_summary(data,old,selected,request,directory),old_note_requests=[n['request'] for n in old],
        old_note_count=len(old),selected_half=selected['half'],request_half=request,directory_half=directory,root_half=root,
        wait_ms=observed,starting_error_ms=abs(observed-WAITS[128]),live_error_ms=abs(observed-WAITS[live]),
        starting_within_2ms=abs(observed-WAITS[128])<=2,live_within_2ms=abs(observed-WAITS[live])<=2,
        configuration=dict(boot_write_counts=config['boot_write_counts'],initial=initial,writes=[]))


def summarize(data,live):
    notes=[e for e in data['events'] if e['register']==0x4C]
    overlap=bool(notes and notes[0]['request']==2)
    result=handoff(data,startup_summary(data,live) if overlap else boundary_summary(data,live))
    writes=result['handoff']['music_writes'];reads=result['operand_reads']
    order='operand_not_observed' if not reads else 'ambiguous_operand'
    if len(reads)==1:
        signs=[w['operand_after_write_ms'] for w in writes]
        order=('missing_music_write' if not signs else 'shared_timestamp' if 0 in signs else
               'all_writes_before_operand' if min(signs)>0 else 'all_writes_after_operand' if max(signs)<0 else 'writes_straddle_operand')
    return dict(**result,startup_overlap=overlap,publication_order=order)


def compare(runs):
    expected=set(cases());by_case={}
    for row in runs:
        if (not isinstance(row,dict) or any(type(row[k]) is not int for k in ('live','gap','spin')) or
                type(row['wait_ms']) not in (int,float) or not 0<=row['wait_ms']<=100 or
                any(type(row[k]) is not bool for k in ('startup_overlap','starting_within_2ms','live_within_2ms'))):
            raise ValueError('invalid publication corpus fields')
        key=tuple(row[k] for k in ('model','live','gap','spin'))
        if key not in expected or key in by_case:raise ValueError('invalid publication corpus coordinate')
        by_case[key]=row
    if set(by_case)!=expected:raise ValueError('incomplete publication corpus')
    pairs=[]
    for model,live,gap,spin in cases():
        if live!=32:continue
        changed=by_case[model,32,gap,spin];control=by_case[model,128,gap,spin]
        pairs.append(dict(model=model,gap=gap,spin=spin,changed_order=changed['publication_order'],
            control_order=control['publication_order'],changed_overlap=changed['startup_overlap'],
            control_overlap=control['startup_overlap'],changed_wait_ms=changed['wait_ms'],control_wait_ms=control['wait_ms'],
            difference_ms=changed['wait_ms']-control['wait_ms'],changed_starting_within_2ms=changed['starting_within_2ms'],
            changed_live_within_2ms=changed['live_within_2ms'],control_starting_within_2ms=control['starting_within_2ms']))
    return dict(parameters_fitted=False,pairs=pairs)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True);parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-tempo-publication-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            def run(case):
                model,live,gap,spin=case;image=build(128,live_tempo=live,change_after=3,boundary=True,request_gap=gap,selection_spin=spin)
                game=root/f'{model}-{live}-{gap}-{spin}.gb';game.write_bytes(image)
                program=args.firmware_dir/('sgb1.program.rom' if model=='sgb' else 'sgb2.program.rom')
                data=capture(args.probe.resolve(),program.resolve(),game,model,300000000,inputs,tempo_publication=True)
                return dict(model=model,live=live,gap=gap,spin=spin,fixture_sha256=hashlib.sha256(image).hexdigest(),**summarize(data,live))
            with ThreadPoolExecutor(max_workers=4) as pool:runs=list(pool.map(run,cases()))
            report=dict(schema='gbb-sgb-vendor-tempo-publication-v1',qualification=False,parameters_fitted=False,
                program_sha256={n:hashlib.sha256((args.firmware_dir/n).read_bytes()).hexdigest() for n in ('sgb1.program.rom','sgb2.program.rom')},
                runs=runs,comparison=compare(runs))
        print(json.dumps(report))
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded tempo-publication experiment failed; child diagnostics suppressed\n')


if __name__=='__main__':main()
