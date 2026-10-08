#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe pending-note KON and inactive countdowns without claiming native support."""
import argparse,csv,io,json,hashlib,subprocess
from pathlib import Path
from build_sgb_score_pending_fixture import bank,build,CASES,DURATIONS
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_reverse_reference import native as reverse_native,validate as reverse_validate
from check_sgb_score_gate_reference import PULSES


def expected(case):
    if case not in CASES:raise ValueError('invalid pending case')
    masks=(12,12,8 if case=='pending-rest' else 12,8,4,4,12,12)
    pitches=((0x98,0x98),(0x99,0x99),(0xA0,0x9A),(0,0x9B),(0x9C,0),(0x9D,0),(0xA3,0xA3),(0xA4,0xA4))
    result=[]
    for i,(mask,pair) in enumerate(zip(masks,pitches)):
        result.append({'mask':mask,'voices':[{'voice':v,'pitch':PITCH[pair[v-2]],'volumes':[7,7] if i<2 or i==2 and v==2 else [1,1],'srcn':2,'adsr1':143,'adsr2':111,'gain':184} for v in (2,3) if mask&(1<<v)]})
    return result


def ticks(case):
    if case not in CASES:raise ValueError('invalid pending case')
    return (0,16,32,48,80,96,112,128) if case=='long-rest-return' else (0,16,32,48,72,88,104,120) if case=='rest-return' else tuple(range(0,128,16))


def onset_contract(result,case):
    if case not in CASES or result.get('keyons')!=expected(case):raise ValueError('pending physical KON/setup differs')
    for edge in result['keyons']:
        if type(edge['mask']) is not int or any(type(value) is not int for voice in edge['voices'] for key,value in voice.items() if key!='volumes') or any(type(value) is not int for voice in edge['voices'] for value in voice['volumes']):raise ValueError('pending setup requires integers')
    intervals=result.get('onset_intervals_spc_cycles');points=ticks(case)
    if not isinstance(intervals,list) or len(intervals)!=7 or any(not integer(v,84000*(b-a)//16,92000*(b-a)//16) for v,a,b in zip(intervals,points,points[1:])):raise ValueError('pending onset interval differs')


def contract(result,case):
    onset_contract(result,case)
    a,d=result.get('articulation'),result.get('pending_duration')
    if type(a) is not int or a not in (63,127) or type(d) is not int or d not in DURATIONS:raise ValueError('invalid pending profile')
    pitches=[{'voice':3,'pitch':1200}]
    if case!='pending-rest':pitches.insert(0,{'voice':2,'pitch':1700})
    if result.get('boundary_pitch_writes')!=pitches or any(type(v) is not int for p in result['boundary_pitch_writes'] for v in p.values()):raise ValueError('pending boundary pitch differs')
    offsets=result.get('boundary_pitch_to_KON_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=len(pitches) or any(not integer(v,0,4096) for v in offsets):raise ValueError('pending pitch timing differs')
    stages=result.get('voice2_volume_updates')
    if not isinstance(stages,list) or len(stages)!=1 or not isinstance(stages[0],dict) or stages[0].get('volumes')!=[1,1] or any(type(v) is not int for v in stages[0]['volumes']) or not integer(stages[0].get('interval_spc_cycles'),168000,300000):raise ValueError('pending deferred DSP volume application differs')
    until_return=sum(result['onset_intervals_spc_cycles'][2:4])
    if not 0<=until_return-stages[0]['interval_spc_cycles']<=4096:raise ValueError('pending volume did not wait for returning note')
    retrigger=case=='note-return'
    identities=[(i,v['voice']) for i,e in enumerate(expected(case)) for v in e['voices'] if not(retrigger and i==2 and v['voice']==2)]
    gates=result.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(identities):raise ValueError('pending missing/extra physical release')
    for g,identity in zip(gates,identities):
        if not isinstance(g,dict) or type(g.get('voice')) is not int or type(g.get('onset_index')) is not int or (g['onset_index'],g['voice'])!=identity or g.get('release_kind')!='keyoff' or not integer(g.get('gate_spc_cycles'),1,400000 if identity==(2,2) else 200000):raise ValueError('pending release identity/bounds differ')
        if identity==(2,2):
            # The release follows returning-rest timing, independently of the
            # pending duration. These are metadata bounds, not new tolerances.
            rest=16 if case=='long-rest-return' else 8
            pulses=PULSES[(96,a,rest)]
            if not 168000+pulses*2048-4096<=g['gate_spc_cycles']<=184000+pulses*2048+4096:raise ValueError('pending gate did not retain the inactive interval')
            offset=sum(result['onset_intervals_spc_cycles'][2:4])
            if not 0<offset-g['gate_spc_cycles']<80000:raise ValueError('pending rest release did not precede returning note')
    rs=result.get('unreleased_retriggers')
    if retrigger:
        if not isinstance(rs,list) or len(rs)!=1 or not isinstance(rs[0],dict) or any(type(rs[0].get(k)) is not int for k in ('voice','onset_index','previous_onset_index')) or (rs[0]['voice'],rs[0]['onset_index'],rs[0]['previous_onset_index'])!=(2,4,2) or not integer(rs[0].get('interval_spc_cycles'),168000,280000) or rs[0]['interval_spc_cycles']!=sum(result['onset_intervals_spc_cycles'][2:4]):raise ValueError('pending unreleased retrigger differs')
    elif rs!=[]:raise ValueError('pending unexpected unreleased retrigger')
    if result.get('unreleased_final_voices')!=[]:raise ValueError('pending unresolved final voice')


def observe(source,case,articulation=127,duration=8):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('pending trace exceeds byte bound')
    result=onsets(io.StringIO(text),case,onset_validator=onset_contract)
    pending={};gates=[];rs=[];index=0;low={};writes=[];write_cycles=[];boundary_kon=None;volumes={};last_pair=None;volume_updates=[]
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        address,value,cycle=(int(row[key]) for key in ('address','value','spc_cycle'))
        if address in (0x20,0x21):
            volumes[address]=value
            if address==0x21 and 0x20 in volumes:
                pair=[volumes[0x20],volumes[0x21]]
                if pair!=last_pair and boundary_kon is not None and 3<=index<=4:
                    volume_updates.append({'volumes':pair,'interval_spc_cycles':cycle-boundary_kon})
                last_pair=pair
        if address in (0x22,0x32):low[address//16]=value
        if address in (0x23,0x33) and index==2:
            voice=address//16
            if voice not in low:raise ValueError('incomplete pending pitch')
            writes.append({'voice':voice,'pitch':low[voice]|value<<8});write_cycles.append(cycle)
        if address==0x5C:
            for voice in (2,3):
                if value&(1<<voice) and voice in pending:
                    onset,start=pending.pop(voice)
                    gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':cycle-start,'release_kind':'keyoff'})
        if address==0x4C and value:
            if index==2:boundary_kon=cycle
            for voice in (2,3):
                if value&(1<<voice):
                    if voice in pending:
                        onset,start=pending[voice]
                        rs.append({'voice':voice,'onset_index':index,'previous_onset_index':onset,'interval_spc_cycles':cycle-start})
                    pending[voice]=(index,cycle)
            index+=1
    result.update(voice2_volume_updates=volume_updates,articulation=articulation,pending_duration=duration,boundary_pitch_writes=writes,boundary_pitch_to_KON_spc_cycles=[boundary_kon-c for c in write_cycles],note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),unreleased_retriggers=rs,unreleased_final_voices=sorted(pending))
    contract(result,case)
    return result


def run(trace,firmware_dir,model,art,duration,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:build(art,duration,name),observer=lambda source:observe(source,case,art,duration),contract=contract,pattern_durations=None)
    result['instruction_limit']=8000000
    return result


def agree(references):
    if not isinstance(references,list) or len(references)!=32:raise ValueError('require complete pending reference matrix')
    seen=set();agreement=[]
    for r in references:
        if not isinstance(r,dict):raise ValueError('invalid pending reference')
        c,a,d,m=(r.get(k) for k in ('case','articulation','pending_duration','model'))
        if c not in CASES or type(a) is not int or a not in (63,127) or type(d) is not int or d not in DURATIONS or m not in ('sgb','sgb2') or (c,a,d,m) in seen:raise ValueError('invalid/duplicate pending reference')
        seen.add((c,a,d,m));contract(r,c)
    for c in CASES:
        for a in (63,127):
            for d in DURATIONS:
                x,y=[r for r in references if r['case']==c and r['articulation']==a and r['pending_duration']==d]
                if x['keyons']!=y['keyons'] or x['boundary_pitch_writes']!=y['boundary_pitch_writes']:raise ValueError('pending original setup differs')
                onset=[abs(p-q) for p,q in zip(x['onset_intervals_spc_cycles'],y['onset_intervals_spc_cycles'])]
                pitch=[abs(p-q) for p,q in zip(x['boundary_pitch_to_KON_spc_cycles'],y['boundary_pitch_to_KON_spc_cycles'])]
                gate=[abs(p['gate_spc_cycles']-q['gate_spc_cycles']) for p,q in zip(x['note_gates'],y['note_gates'])]
                volume=[abs(p['interval_spc_cycles']-q['interval_spc_cycles']) for p,q in zip(x['voice2_volume_updates'],y['voice2_volume_updates'])]
                rs=[abs(p['interval_spc_cycles']-q['interval_spc_cycles']) for p,q in zip(x['unreleased_retriggers'],y['unreleased_retriggers'])]
                if any(v>2048 for v in onset+pitch+gate+rs+volume):raise ValueError('pending originals exceed retained intermodel allowance')
                agreement.append({'case':c,'articulation':a,'pending_duration':d,'absolute_onset_difference_spc_cycles':onset,'absolute_pitch_difference_spc_cycles':pitch,'absolute_gate_difference_spc_cycles':gate,'absolute_retrigger_difference_spc_cycles':rs,'absolute_volume_difference_spc_cycles':volume})
    # The returning rest, rather than the pending note duration, governs the
    # observed release in these paired-duration fixtures on each model.
    for c in ('rest-return','long-rest-return'):
        for a in (63,127):
            for m in ('sgb','sgb2'):
                x,y=[r for r in references if r['case']==c and r['articulation']==a and r['model']==m]
                gx=next(g['gate_spc_cycles'] for g in x['note_gates'] if (g['onset_index'],g['voice'])==(2,2))
                gy=next(g['gate_spc_cycles'] for g in y['note_gates'] if (g['onset_index'],g['voice'])==(2,2))
                if abs(gx-gy)>2048:raise ValueError('returning rest release depends on pending duration')
    return agreement


def rejection(report):
    reverse_validate(report)
    if report['status']!=226 or report['events'] or report['keyons'] or report['keyoffs'] or report['instrument_writes'] or report['voice_writes'] or report['pcm']['nonzero_frames'] or report['pcm']['peak'] or not report['source_unmodified'] or not report['cache_guards_equal']:raise ValueError('pending fixture bypassed conservative native guard')


def native_rejections(probe):
    result={}
    for c in CASES:
        for a in (63,127):
            for d in DURATIONS:
                r=reverse_native(probe,bank(a,d,c));rejection(r);result[f'{c}-{a}-{d}']=r
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);p.add_argument('--probe',type=Path);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for d in DURATIONS for a in (63,127) for m in ('sgb','sgb2')]
        result={'schema':'gbb-score-pending-observation-v1','qualification':False,'playback':False,'native_supported':False,'reference':refs,'intermodel_agreement':agree(refs)}
        if args.probe:
            from build_sgb_score_reverse import build as program_build
            result.update(rejected_native=native_rejections(args.probe.resolve()),native_program_sha256=hashlib.sha256(program_build()).hexdigest())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
