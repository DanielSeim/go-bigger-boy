#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opaque following-rest observations with owned inputs and bounded DSP metadata."""
import argparse,csv,io,json,hashlib,subprocess
from pathlib import Path
from build_sgb_score_follow_rest_fixture import bank,build,CASES,DURATIONS
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_gate_reference import PULSES


def expected(case):
    if case not in CASES:raise ValueError('invalid following-rest case')
    masks=(12,12,12 if case=='boundary-note' else 8,12,4,4,12,12)
    pitches=((0x98,0x98),(0x99,0x99),(0xA0,0x9A),(0x9C,0x9B),(0x9D,0),(0x9E,0),(0xA3,0xA3),(0xA4,0xA4))
    return [{'mask':mask,'voices':[{'voice':v,'pitch':PITCH[pair[v-2]],'volumes':[7,7] if i<2 or i==2 and v==2 else [1,1],'srcn':2,'adsr1':143,'adsr2':111,'gain':184} for v in (2,3) if mask&(1<<v)]} for i,(mask,pair) in enumerate(zip(masks,pitches))]


def ticks(rest):
    if type(rest) is not int or rest not in DURATIONS:raise ValueError('invalid following rest duration')
    return (0,16,32,32+rest,48+rest,64+rest,80+rest,96+rest)


def contract(r,case):
    a,d,s,b=(r.get(k) for k in ('articulation','boundary_duration','rest_duration','rest_articulation'))
    if any(type(v) is not int or v not in (63,127) for v in (a,b)) or any(type(v) is not int or v not in DURATIONS for v in (d,s)) or case not in CASES:raise ValueError('invalid following-rest profile')
    if r.get('keyons')!=expected(case):raise ValueError('following-rest physical KON differs')
    for e in r['keyons']:
        if type(e['mask']) is not int or any(type(x) is not int for v in e['voices'] for k,x in v.items() if k!='volumes') or any(type(x) is not int for v in e['voices'] for x in v['volumes']):raise ValueError('following-rest setup must contain integers')
    points=ticks(s);values=r.get('onset_intervals_spc_cycles')
    if not isinstance(values,list) or len(values)!=7 or any(not integer(v,84000*(y-x)//16,92000*(y-x)//16) for v,x,y in zip(values,points,points[1:])):raise ValueError('following-rest onset intervals differ')
    wanted=[(i,v['voice']) for i,e in enumerate(expected(case)) for v in e['voices']]
    gates=r.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(wanted):raise ValueError('following-rest release count differs')
    for g,target in zip(gates,wanted):
        if not isinstance(g,dict) or type(g.get('voice')) is not int or type(g.get('onset_index')) is not int or (g['onset_index'],g['voice'])!=target or g.get('release_kind')!='keyoff' or not integer(g.get('gate_spc_cycles'),1,200000):raise ValueError('following-rest release identity differs')
        duration=s if target[0]==2 else 16;pulses=PULSES[(96,b if target[0] in (2,3) else a,duration)]
        if not 2048*(pulses-2)<=g['gate_spc_cycles']<=2048*pulses+4096:raise ValueError('following-rest release profile differs')
    if r.get('unreleased_retriggers')!=[] or r.get('unreleased_final_voices')!=[]:raise ValueError('following-rest note lacks physical release')
    pitches=[{'voice':3,'pitch':1200}]
    if case=='boundary-note':pitches.insert(0,{'voice':2,'pitch':1700})
    if r.get('boundary_pitch_writes')!=pitches or any(type(v) is not int for p in r['boundary_pitch_writes'] for v in p.values()):raise ValueError('following-rest boundary pitch differs')
    offsets=r.get('boundary_pitch_to_KON_spc_cycles')
    if not isinstance(offsets,list) or len(offsets)!=len(pitches) or any(not integer(v,0,4096) for v in offsets):raise ValueError('following-rest pitch timing differs')
    updates=r.get('voice2_volume_updates')
    if not isinstance(updates,list) or len(updates)!=1 or not isinstance(updates[0],dict) or updates[0].get('volumes')!=[1,1] or any(type(v) is not int for v in updates[0]['volumes']) or not integer(updates[0].get('interval_spc_cycles'),1,100000) or not 0<=values[2]-updates[0]['interval_spc_cycles']<=4096:raise ValueError('following-rest volume did not wait for next note')


def observe(source,case,articulation=127,duration=8,rest_duration=8,rest_articulation=None):
    rest_articulation=articulation if rest_articulation is None else rest_articulation
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('following-rest trace exceeds byte bound')
    r=onsets(io.StringIO(text),case,onset_validator=lambda r,c:None)
    pending={};gates=[];rs=[];index=0;low={};writes=[];cycles=[];kon=None;volumes={};last_pair=None;updates=[]
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        address,value,cycle=(int(row[k]) for k in ('address','value','spc_cycle'))
        if address in (0x20,0x21):
            volumes[address]=value
            if address==0x21 and 0x20 in volumes:
                pair=[volumes[0x20],volumes[0x21]]
                if pair!=last_pair and kon is not None and index==3:updates.append({'volumes':pair,'interval_spc_cycles':cycle-kon})
                last_pair=pair
        if address in (0x22,0x32):low[address//16]=value
        if address in (0x23,0x33) and index==2:
            voice=address//16
            if voice not in low:raise ValueError('incomplete following-rest pitch')
            writes.append({'voice':voice,'pitch':low[voice]|value<<8});cycles.append(cycle)
        if address==0x5C:
            for voice in (2,3):
                if value&(1<<voice) and voice in pending:
                    onset,start=pending.pop(voice);gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':cycle-start,'release_kind':'keyoff'})
        if address==0x4C and value:
            if index==2:kon=cycle
            for voice in (2,3):
                if value&(1<<voice):
                    if voice in pending:rs.append(voice)
                    pending[voice]=(index,cycle)
            index+=1
    r.update(rest_articulation=rest_articulation,articulation=articulation,boundary_duration=duration,rest_duration=rest_duration,note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),unreleased_retriggers=rs,unreleased_final_voices=sorted(pending),boundary_pitch_writes=writes,boundary_pitch_to_KON_spc_cycles=[kon-c for c in cycles],voice2_volume_updates=updates)
    contract(r,case);return r


def run(trace,firmware_dir,model,art,duration,rest,case,rest_articulation=None):
    r=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda _:build(art,duration,rest,case,rest_articulation),observer=lambda source:observe(source,case,art,duration,rest,rest_articulation),contract=contract,pattern_durations=None)
    r['instruction_limit']=8000000;return r


def agree(references):
    if not isinstance(references,list) or len(references)!=64:raise ValueError('require complete following-rest reference matrix')
    seen=set();result=[]
    for r in references:
        if not isinstance(r,dict):raise ValueError('invalid following-rest reference')
        c,a,d,s,b,m=(r.get(k) for k in ('case','articulation','boundary_duration','rest_duration','rest_articulation','model'))
        contract(r,c)
        if m not in ('sgb','sgb2') or (c,a,d,s,b,m) in seen:raise ValueError('duplicate/invalid following-rest reference')
        seen.add((c,a,d,s,b,m))
    for c in CASES:
        for a in (63,127):
            for d in DURATIONS:
                for s in DURATIONS:
                    for b in (63,127):
                        pair=[r for r in references if (r['case'],r['articulation'],r['boundary_duration'],r['rest_duration'],r['rest_articulation'])==(c,a,d,s,b)]
                        if len(pair)!=2:raise ValueError('missing following-rest intermodel pair')
                        x,y=pair
                        if x['keyons']!=y['keyons'] or x['boundary_pitch_writes']!=y['boundary_pitch_writes']:raise ValueError('following-rest intermodel setup differs')
                        onset=[abs(p-q) for p,q in zip(x['onset_intervals_spc_cycles'],y['onset_intervals_spc_cycles'])]
                        gate=[abs(p['gate_spc_cycles']-q['gate_spc_cycles']) for p,q in zip(x['note_gates'],y['note_gates'])]
                        pitch=[abs(p-q) for p,q in zip(x['boundary_pitch_to_KON_spc_cycles'],y['boundary_pitch_to_KON_spc_cycles'])]
                        volume=abs(x['voice2_volume_updates'][0]['interval_spc_cycles']-y['voice2_volume_updates'][0]['interval_spc_cycles'])
                        if any(v>2048 for v in onset+gate+pitch+[volume]):raise ValueError('following-rest intermodel allowance exceeded')
                        result.append({'case':c,'articulation':a,'boundary_duration':d,'rest_duration':s,'rest_articulation':b,'absolute_onset_difference_spc_cycles':onset,'absolute_gate_difference_spc_cycles':gate,'absolute_pitch_difference_spc_cycles':pitch,'absolute_volume_difference_spc_cycles':volume})
    # Crossed durations AND articulations rule out retaining the boundary
    # note's profile: hold the following rest fixed across all four origins.
    for s in DURATIONS:
        for b in (63,127):
            for m in ('sgb','sgb2'):
                group=[r for r in references if (r['case'],r['rest_duration'],r['rest_articulation'],r['model'])==('boundary-note',s,b,m)]
                gates=[next(g['gate_spc_cycles'] for g in r['note_gates'] if (g['onset_index'],g['voice'])==(2,2)) for r in group]
                if len(gates)!=4 or max(gates)-min(gates)>2048:raise ValueError('following-rest release depends on boundary profile')
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,d,s,c,b) for c in CASES for d in DURATIONS for s in DURATIONS for a in (63,127) for b in (63,127) for m in ('sgb','sgb2')]
        result={'schema':'gbb-score-follow-rest-observation-v1','qualification':False,'playback':False,'reference':refs,'intermodel_agreement':agree(refs)}
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
