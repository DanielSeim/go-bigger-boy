#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque observations of a ready note/rest at final channel-3 end."""
import argparse,csv,io,json,hashlib,subprocess
from pathlib import Path
from build_sgb_score_final_fixture import bank,build,CASES,DURATIONS
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_gate_reference import PULSES


def expected(case):
    if case not in CASES:raise ValueError('invalid final case')
    pitches=(1068,1132,1700) if case=='final-note' else (1068,1132)
    return [{'mask':4 if i==2 else 12,'voices':[{'voice':v,'pitch':pitch,'volumes':[7,7],'srcn':2,'adsr1':143,'adsr2':111,'gain':184} for v in ((2,) if i==2 else (2,3))]} for i,pitch in enumerate(pitches)]


def contract(r,case):
    a,d=(r.get(k) for k in ('articulation','boundary_duration'))
    if type(a) is not int or a not in (63,127) or type(d) is not int or d not in DURATIONS or case not in CASES:raise ValueError('invalid final profile')
    if r.get('keyons')!=expected(case):raise ValueError('final actual KON/setup differs')
    for e in r['keyons']:
        if type(e['mask']) is not int or any(type(x) is not int for v in e['voices'] for k,x in v.items() if k!='volumes') or any(type(x) is not int for v in e['voices'] for x in v['volumes']):raise ValueError('invalid final setup types')
    intervals=r.get('onset_intervals_spc_cycles')
    if not isinstance(intervals,list) or len(intervals)!=(2 if case=='final-note' else 1) or any(not integer(v,84000,92000) for v in intervals):raise ValueError('final onset interval differs')
    if not integer(r.get('final_stop_interval_spc_cycles'),84000,92000):raise ValueError('final stop tick differs')
    wanted=[(0,2),(0,3),(1,2),(1,3)];gates=r.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=4:raise ValueError('final preceding releases differ')
    pulses=PULSES[(96,a,16)]
    for g,identity in zip(gates,wanted):
        if not isinstance(g,dict) or type(g.get('voice')) is not int or type(g.get('onset_index')) is not int or (g['onset_index'],g['voice'])!=identity or g.get('release_kind')!='keyoff' or not integer(g.get('gate_spc_cycles'),2048*(pulses-2),2048*pulses+4096):raise ValueError('final preceding gate differs')
    controls=r.get('final_control_writes');wanted=[{'address':0x5C,'value':255},{'address':0x5C,'value':0},{'address':0x4C,'value':4 if case=='final-note' else 0}]
    if controls!=wanted or any(type(v) is not int for w in controls for v in w.values()):raise ValueError('final KOF/KON ordering differs')
    offsets=r.get('final_control_offsets_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=3 or offsets[0]!=0 or any(not integer(v,0,4096) for v in offsets) or not offsets[0]<offsets[1]<offsets[2]:raise ValueError('final stop write timing differs')
    pitch=[{'voice':2,'pitch':1700}] if case=='final-note' else []
    if r.get('boundary_pitch_writes')!=pitch or any(type(v) is not int for p in r['boundary_pitch_writes'] for v in p.values()):raise ValueError('final boundary pitch differs')
    po=r.get('boundary_pitch_to_stop_spc_cycles')
    if not isinstance(po,list) or len(po)!=len(pitch) or any(not integer(v,1,4096) for v in po):raise ValueError('final boundary pitch timing differs')
    if r.get('post_second_onset_volume_writes')!=[]:raise ValueError('final stop applied deferred volume')
    if r.get('unreleased_retriggers')!=[] or r.get('unreleased_final_voices')!=([2] if case=='final-note' else []):raise ValueError('final held voice differs')
    if r.get('final_volumes')!=[[7,7],[7,7]] or any(type(v) is not int for pair in r['final_volumes'] for v in pair):raise ValueError('final actual volume differs')
    held=r.get('held_observation_spc_cycles')
    if not integer(held,200000,6000000):raise ValueError('final post-stop observation too short or unbounded')


def observe(source,case,articulation=127,duration=8):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('final trace exceeds byte bound')
    r=onsets(io.StringIO(text),case,onset_validator=lambda r,c:None)
    rows=[];onset_cycles=[];pending={};gates=[];retriggers=[];regs={};index=0;last_cycle=0
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        address,value,cycle=(int(row[k]) for k in ('address','value','spc_cycle'));last_cycle=cycle
        rows.append((address,value,cycle));regs[address]=value
        if address==0x5C:
            for voice in (2,3):
                if value&(1<<voice) and voice in pending:
                    onset,start=pending.pop(voice);gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':cycle-start,'release_kind':'keyoff'})
        if address==0x4C and value:
            for voice in (2,3):
                if value&(1<<voice):
                    if voice in pending:retriggers.append(voice)
                    pending[voice]=(index,cycle)
            onset_cycles.append(cycle);index+=1
    if len(onset_cycles)<2:raise ValueError('missing final reference setup')
    stops=[(i,c) for i,(a,v,c) in enumerate(rows) if a==0x5C and v==255 and c>onset_cycles[1]]
    if len(stops)!=1:raise ValueError('missing/extra final stop')
    stop_index,stop=stops[0];controls=[(a,v,c) for a,v,c in rows[stop_index:] if a in (0x4C,0x5C)][:3]
    writes=[];wc=[];low={};volume_writes=[]
    for a,v,c in rows:
        if a in (0x20,0x21,0x30,0x31) and c>onset_cycles[1]:volume_writes.append({'address':a,'value':v})
        if a in (0x22,0x32):low[a//16]=v
        if a in (0x23,0x33) and onset_cycles[1]<c<stop:
            voice=a//16
            if voice not in low:raise ValueError('incomplete final pitch')
            writes.append({'voice':voice,'pitch':low[voice]|v<<8});wc.append(c)
    r.update(articulation=articulation,boundary_duration=duration,note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),unreleased_retriggers=retriggers,unreleased_final_voices=sorted(pending),final_stop_interval_spc_cycles=stop-onset_cycles[1],final_control_writes=[{'address':a,'value':v} for a,v,c in controls],final_control_offsets_spc_cycles=[c-stop for a,v,c in controls],boundary_pitch_writes=writes,boundary_pitch_to_stop_spc_cycles=[stop-c for c in wc],post_second_onset_volume_writes=volume_writes,final_volumes=[[regs[0x20],regs[0x21]],[regs[0x30],regs[0x31]]],held_observation_spc_cycles=last_cycle-(onset_cycles[2] if len(onset_cycles)==3 else stop))
    contract(r,case);return r


def run(trace,firmware_dir,model,art,duration,case):
    r=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(art,duration,case),observer=lambda source:observe(source,case,art,duration),contract=contract,pattern_durations=None)
    r['instruction_limit']=8000000;return r


def agree(refs):
    if not isinstance(refs,list) or len(refs)!=16:raise ValueError('require complete final reference matrix')
    seen=set();result=[]
    for r in refs:
        if not isinstance(r,dict):raise ValueError('invalid final reference')
        c,a,d,m=(r.get(k) for k in ('case','articulation','boundary_duration','model'));contract(r,c)
        if m not in ('sgb','sgb2') or (c,a,d,m) in seen:raise ValueError('invalid/duplicate final reference')
        seen.add((c,a,d,m))
    for c in CASES:
        for a in (63,127):
            for d in DURATIONS:
                x,y=[r for r in refs if (r['case'],r['articulation'],r['boundary_duration'])==(c,a,d)]
                onset=[abs(p-q) for p,q in zip(x['onset_intervals_spc_cycles'],y['onset_intervals_spc_cycles'])]
                gates=[abs(p['gate_spc_cycles']-q['gate_spc_cycles']) for p,q in zip(x['note_gates'],y['note_gates'])]
                pitch=[abs(p-q) for p,q in zip(x['boundary_pitch_to_stop_spc_cycles'],y['boundary_pitch_to_stop_spc_cycles'])]
                controls=[abs(p-q) for p,q in zip(x['final_control_offsets_spc_cycles'],y['final_control_offsets_spc_cycles'])]
                stop=abs(x['final_stop_interval_spc_cycles']-y['final_stop_interval_spc_cycles'])
                if any(v>2048 for v in onset+gates+pitch+controls+[stop]):raise ValueError('final originals exceed retained allowance')
                result.append({'case':c,'articulation':a,'boundary_duration':d,'absolute_onset_difference_spc_cycles':onset,'absolute_gate_difference_spc_cycles':gates,'absolute_pitch_difference_spc_cycles':pitch,'absolute_stop_difference_spc_cycles':stop,'absolute_control_difference_spc_cycles':controls})
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,c) for c in CASES for d in DURATIONS for a in (63,127) for m in ('sgb','sgb2')]
        result={'schema':'gbb-score-final-observation-v1','qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
