#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque ordering evidence and channel-2-first native qualification."""
import argparse,csv,io,json,hashlib,subprocess
from pathlib import Path
from build_sgb_score_order_fixture import bank,build,expected,CASES,QUALIFIED_CASES
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_peer_reference import validate as peer_validate,align as peer_align,native_observation,SCHEMA as PEER_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native
SCHEMA='gbb-spc-score-order-v1'


def onset_contract(result,case):
    if case not in CASES or result.get('keyons')!=expected(case):raise ValueError('owned ordering onsets differ')
    # JSON bools must not satisfy integer metadata contracts.
    for edge in result['keyons']:
        if type(edge['mask']) is not int or any(type(value) is not int for voice in edge['voices'] for key,value in voice.items() if key!='volumes') or any(type(value) is not int for voice in edge['voices'] for value in voice['volumes']):raise ValueError('ordering setup must contain integers')
    intervals=result.get('onset_intervals_spc_cycles')
    if not isinstance(intervals,list) or len(intervals)!=7 or any(not integer(v,84000,92000) for v in intervals):raise ValueError('ordering onset intervals differ')


def contract(result,case):
    onset_contract(result,case)
    pitches=[{'voice':2,'pitch':1200}]
    if case=='end-3-note':pitches.insert(0,{'voice':2,'pitch':1700})
    if result.get('boundary_pitch_writes')!=pitches:raise ValueError('same-tick pitch write ordering differs')
    if any(type(v) is not int for p in result['boundary_pitch_writes'] for v in p.values()):raise ValueError('invalid ordering pitch types')
    wanted=[(i,v['voice']) for i,e in enumerate(expected(case)) for v in e['voices']]
    gates=result.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(wanted) or any(not isinstance(g,dict) or type(g.get('voice')) is not int or type(g.get('onset_index')) is not int or (g['onset_index'],g['voice'])!=target or not integer(g.get('gate_spc_cycles'),1,200000) or g.get('release_kind')!='keyoff' for g,target in zip(gates,wanted)):raise ValueError('ordering releases differ')
    if result.get('unreleased_retriggers')!=[] or result.get('unreleased_final_voices')!=[]:raise ValueError('ordering unexpected held voice')


def observe(source,case):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('ordering trace exceeds byte bound')
    result=onsets(io.StringIO(text),case,onset_validator=onset_contract)
    pending={};gates=[];retriggers=[];index=0;pitch_low={};writes=[]
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        address,value,cycle=(int(row[key]) for key in ('address','value','spc_cycle'))
        if address in (0x22,0x32):pitch_low[address//16]=value
        if address in (0x23,0x33) and index==2:
            voice=address//16
            if voice not in pitch_low:raise ValueError('incomplete boundary pitch')
            writes.append({'voice':voice,'pitch':pitch_low[voice]|value<<8})
        if address==0x5C:
            for voice in (2,3):
                if value&(1<<voice) and voice in pending:
                    onset,start=pending.pop(voice)
                    gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':cycle-start,'release_kind':'keyoff'})
        if address==0x4C and value:
            for voice in (2,3):
                if value&(1<<voice):
                    if voice in pending:retriggers.append(voice)
                    pending[voice]=(index,cycle)
            index+=1
    result.update(boundary_pitch_writes=writes,note_gates=sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),unreleased_retriggers=retriggers,unreleased_final_voices=sorted(pending))
    contract(result,case)
    return result


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:build(art,name),observer=lambda source:observe(source,case),contract=contract,pattern_durations=None)
    result.update(articulation=art,instruction_limit=8000000)
    return result


def validate(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA:raise ValueError('invalid ordering schema')
    peer_validate({**report,'schema':PEER_SCHEMA})


def align(report,data):
    validate(report);peer_align({**report,'schema':PEER_SCHEMA},data)


def native(probe,data,tempo=96):
    from build_sgb_score_order import build as program_build
    return gate_native(probe,data,tempo,builder=program_build,validator=validate,output_bound=131072)


def compare(candidates,references):
    keys={f'{c}-{a}' for c in QUALIFIED_CASES for a in (63,127)}
    if not isinstance(candidates,dict) or set(candidates)!=keys or not isinstance(references,list) or len(references)!=16:raise ValueError('require complete ordering matrix')
    seen=set();comparisons=[]
    for ref in references:
        if not isinstance(ref,dict):raise ValueError('invalid ordering reference')
        c,a,m=(ref.get(k) for k in ('case','articulation','model'))
        if c not in CASES or type(a) is not int or a not in (63,127) or m not in ('sgb','sgb2') or (c,a,m) in seen:raise ValueError('invalid/duplicate ordering reference')
        seen.add((c,a,m));contract(ref,c)
        if c not in QUALIFIED_CASES:continue
        report=candidates[f'{c}-{a}'];align(report,bank(a,c))
        if len(report['keyons'])!=8:raise ValueError('unexpected ordering KON')
        for edge,target in zip(report['keyons'],expected(c)):
            if edge['mask']!=target['mask'] or any(edge['pitches'][v['voice']-2]!=v['pitch'] or edge['volumes'][v['voice']-2]!=v['volumes'] for v in target['voices']):raise ValueError('native ordering setup differs')
        gates,rs=native_observation(report);original=ref['note_gates']
        if rs or len(gates)!=12 or any((g['onset_index'],g['voice'])!=(o['onset_index'],o['voice']) or g['cause']!=1 for g,o in zip(gates,original)):raise ValueError('native ordering release identities differ')
        differences=[abs(g['gate_spc_cycles']-o['gate_spc_cycles']) for g,o in zip(gates,original)]
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        onset_diff=[abs(a-b) for a,b in zip(intervals,ref['onset_intervals_spc_cycles'])]
        if any(v>4096 for v in differences+onset_diff):raise ValueError('ordering exceeds retained allowance')
        comparisons.append({'case':c,'articulation':a,'model':m,'absolute_gate_difference_spc_cycles':differences,'absolute_onset_difference_spc_cycles':onset_diff})
    for c in CASES:
        for a in (63,127):
            x,y=[r for r in references if r['case']==c and r['articulation']==a]
            if x['keyons']!=y['keyons'] or x['boundary_pitch_writes']!=y['boundary_pitch_writes'] or any(abs(p-q)>2048 for p,q in zip(x['onset_intervals_spc_cycles'],y['onset_intervals_spc_cycles'])):raise ValueError('ordering originals disagree')
    return comparisons


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);p.add_argument('--probe',type=Path);args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),m,a,c) for c in CASES for a in (63,127) for m in ('sgb','sgb2')]
        result={'schema':'gbb-score-order-reference-v1','qualification':False,'playback':False,'reference':refs,'qualified_cases':list(QUALIFIED_CASES),'observation_only_cases':list(CASES[2:])}
        if args.probe:
            from build_sgb_score_order import build as program_build
            candidates={f'{c}-{a}':native(args.probe.resolve(),bank(a,c)) for c in QUALIFIED_CASES for a in (63,127)}
            result.update(native=candidates,comparisons=compare(candidates,refs),program_sha256=hashlib.sha256(program_build()).hexdigest())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))
