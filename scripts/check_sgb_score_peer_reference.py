#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded opaque observation of clipped peers and rest reactivation."""
import argparse,csv,io,json,hashlib,subprocess
from pathlib import Path
from build_sgb_score_peer_fixture import bank,build,expected,ticks,CASES
from check_sgb_score_sparse_reference import onsets
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from check_sgb_score_reselect_reference import validate as reselect_validate,SCHEMA as RESELECT_SCHEMA
from check_sgb_score_polygate_reference import align as gate_align,native as gate_native
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_mix_reference import selected
from schedule_sgb_score import schedule

def observe(source,case):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024:raise ValueError('tail trace exceeds byte bound')
    result=onsets(io.StringIO(text),case,onset_validator=contract)
    pending={};gates=[];retriggers=[];index=0
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        address,value,cycle=(int(row[key]) for key in ('address','value','spc_cycle'))
        if address==0x5C:
            for voice in (2,3):
                if value&(1<<voice) and voice in pending:
                    onset,start=pending.pop(voice)
                    gates.append({'voice':voice,'onset_index':onset,'gate_spc_cycles':cycle-start,'release_kind':'keyoff'})
        if address==0x4C and value:
            for voice in (2,3):
                if value&(1<<voice):
                    if voice in pending:
                        onset,start=pending[voice]
                        retriggers.append({'voice':voice,'onset_index':index,'previous_onset_index':onset,'interval_spc_cycles':cycle-start})
                    pending[voice]=(index,cycle)
            index+=1
    result.update(note_gates=sorted(gates,key=lambda gate:(gate['onset_index'],gate['voice'])),unreleased_retriggers=retriggers,unreleased_final_voices=sorted(pending))
    return result


def contract(result,case):
    if result['keyons']!=expected(case):raise ValueError('peer owned onset contract differs')
    for edge in result['keyons']:
        if type(edge['mask']) is not int or any(type(value) is not int for voice in edge['voices'] for key,value in voice.items() if key!='volumes') or any(type(value) is not int for voice in edge['voices'] for value in voice['volumes']):raise ValueError('peer setup must contain integers')
    intervals=result['onset_intervals_spc_cycles'];points=ticks(case)
    if len(intervals)!=7 or any(not integer(v,84000*(b-a)//16,92000*(b-a)//16) for v,a,b in zip(intervals,points,points[1:])):raise ValueError('peer onset intervals differ')

    if 'articulation' not in result:return
    missing=case.startswith('clip') and result['articulation']==127
    peer=5-int(case[-1]);wanted=[(i,v['voice']) for i,e in enumerate(expected(case)) for v in e['voices'] if not(missing and i==1 and v['voice']==peer)]
    gates=result.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(wanted) or any(not isinstance(g,dict) or type(g.get('voice')) is not int or type(g.get('onset_index')) is not int or (g['onset_index'],g['voice'])!=target or not integer(g.get('gate_spc_cycles'),1,400000) or g.get('release_kind')!='keyoff' for g,target in zip(gates,wanted)):raise ValueError('peer directly observed gates differ')
    rs=result.get('unreleased_retriggers')
    if missing:
        onset=4 if peer==3 else 2
        if not isinstance(rs,list) or len(rs)!=1 or not isinstance(rs[0],dict) or any(type(rs[0].get(k)) is not int for k in ('voice','onset_index','previous_onset_index')) or (rs[0]['voice'],rs[0]['onset_index'],rs[0]['previous_onset_index'])!=(peer,onset,1) or not integer(rs[0].get('interval_spc_cycles'),40000,400000):raise ValueError('peer unreleased retrigger differs')
    elif rs!=[]:raise ValueError('peer unexpected unreleased retrigger')
    if result.get('unreleased_final_voices')!=[]:raise ValueError('peer ended with unresolved voice')


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:build(art,name),observer=lambda source:observe(source,case),contract=contract,pattern_durations=None)
    result.update(articulation=art,instruction_limit=8000000)
    contract(result,case)
    return result


SCHEMA='gbb-spc-score-peer-v1'


def validate(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA:raise ValueError('invalid trailing-control schema')
    reselect_validate({**report,'schema':RESELECT_SCHEMA},clipped_peer=True)
    checks=report.get('frozen_peer_checks')
    if not isinstance(checks,list) or len(checks)!=2 or any(not integer(c,0,30000000) for c in checks):raise ValueError('invalid frozen peer observations')


def align(report,data):
    validate(report)
    gate_align(report,data,schema=SCHEMA,max_events=64,max_ticks=2032,pitch_table=PITCH,
               isolated_voices=False,pattern_ticks=report['pattern_ticks'],sparse=True,clipped_peer=True,inherit_timing=True,end_priority=True)
    symbolic=schedule(data,int.from_bytes(data[:2],'little'),inherit_timing=True,end_priority=True)
    if report['pattern_masks']!=[sum(1<<channel for channel in pattern['channels']) for pattern in symbolic['patterns']]:
        raise ValueError('inheritance pattern masks differ')
    controls={2:[10,127],3:[10,127]};counts={2:0,3:0};song=160;index=0
    for event in symbolic['events']:
        channel,kind=event['channel'],event['kind']
        if kind=='pan':controls[channel][0]=event['value']
        elif kind=='track_volume':controls[channel][1]=event['value']
        elif kind=='song_volume':song=event['value']
        elif kind=='instrument':counts[channel]+=1
        elif kind in ('note','rest'):
            actual=report['events'][index];index+=1
            pan,track=controls[channel]
            if [actual['pan'],actual['track_volume'],actual['song_volume'],actual['volumes'],actual['instrument_sets']]!=[pan,track,song,selected(pan,track,song,kind=='rest'),counts[channel]]:
                raise ValueError('native inheritance differs from executed symbolic controls')
            counts[channel]=0
    if index!=len(report['events']):raise ValueError('inheritance event count differs')


def native(probe,data,tempo=96):
    from build_sgb_score_peer import build
    return gate_native(probe,data,tempo,builder=build,validator=validate,output_bound=131072)


def native_observation(report):
    gates=[];pending={};retriggers=[];index=0
    for half,is_on,edge in sorted([(e['half_cycle'],True,e) for e in report['keyons']]+[(e['half_cycle'],False,e) for e in report['keyoffs']]):
        for voice in (2,3):
            if not edge['mask']&(1<<voice):continue
            if is_on:
                if voice in pending:
                    old,start=pending[voice]
                    retriggers.append({'voice':voice,'onset_index':index,'previous_onset_index':old,'interval_spc_cycles':(half-start)/2})
                pending[voice]=(index,half)
            else:
                old,start=pending.pop(voice)
                gates.append({'voice':voice,'onset_index':old,'gate_spc_cycles':(half-start)/2,'cause':edge['cause']})
        if is_on:index+=1
    if pending:raise ValueError('native peer finished without a release')
    return sorted(gates,key=lambda g:(g['onset_index'],g['voice'])),retriggers


def compare(candidates,references):
    keys={f'{case}-{art}' for case in CASES for art in (63,127)}
    if not isinstance(candidates,dict) or set(candidates)!=keys or not isinstance(references,list) or len(references)!=16:raise ValueError('require peer cases/articulations/both originals')
    seen=set();comparisons=[]
    for reference in references:
        if not isinstance(reference,dict):raise ValueError('invalid peer reference')
        case,art,model=(reference.get(key) for key in ('case','articulation','model'))
        if case not in CASES or type(art) is not int or art not in (63,127) or model not in ('sgb','sgb2') or (case,art,model) in seen:raise ValueError('invalid/duplicate peer reference')
        seen.add((case,art,model));contract(reference,case)
        report=candidates[f'{case}-{art}'];align(report,bank(art,case))
        if case[-1]=='2' and art==127 and not report['frozen_peer_checks'][1]:raise ValueError('delayed clipped peer was not observed frozen')
        if len(report['keyons'])!=8:raise ValueError('peer onset count differs')
        for edge,target in zip(report['keyons'],expected(case)):
            if edge['mask']!=target['mask'] or any(edge['pitches'][v['voice']-2]!=v['pitch'] or edge['volumes'][v['voice']-2]!=v['volumes'] for v in target['voices']):raise ValueError('peer physical onset differs')
        gates,retriggers=native_observation(report);original=reference['note_gates']
        if len(gates)!=len(original) or any((a['voice'],a['onset_index'])!=(b['voice'],b['onset_index']) or a['cause']!=1 for a,b in zip(gates,original)):raise ValueError('peer gates differ or scheduler fabricated release')
        differences=[abs(a['gate_spc_cycles']-b['gate_spc_cycles']) for a,b in zip(gates,original)]
        if any(v>4096 for v in differences):raise ValueError('peer gate exceeds retained allowance')
        wanted=reference['unreleased_retriggers']
        if len(retriggers)!=len(wanted) or any(any(a[key]!=b[key] for key in ('voice','onset_index','previous_onset_index')) or abs(a['interval_spc_cycles']-b['interval_spc_cycles'])>4096 for a,b in zip(retriggers,wanted)):raise ValueError('unreleased retrigger differs')
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        if any(abs(a-b)>4096 for a,b in zip(intervals,reference['onset_intervals_spc_cycles'])):raise ValueError('peer onset exceeds retained allowance')
        comparisons.append({'case':case,'articulation':art,'model':model,'absolute_gate_difference_spc_cycles':differences,'native_DSP_intervals_spc_cycles':intervals,'reference_DSP_intervals_spc_cycles':reference['onset_intervals_spc_cycles'],'native_unreleased_retriggers':retriggers})
    for case in CASES:
        for art in (63,127):
            a,b=[r for r in references if r['case']==case and r['articulation']==art]
            if a['keyons']!=b['keyons'] or any(abs(x-y)>2048 for x,y in zip(a['onset_intervals_spc_cycles'],b['onset_intervals_spc_cycles'])):raise ValueError('peer originals disagree')
    return comparisons


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--trace',type=Path,required=True);p.add_argument('--firmware-dir',type=Path,required=True);p.add_argument('--probe',type=Path)
    args=p.parse_args()
    try:
        refs=[run(args.trace.resolve(),args.firmware_dir.resolve(),model,art,case) for case in CASES for art in (63,127) for model in ('sgb','sgb2')]
        result={'schema':'gbb-score-peer-reference-v1','qualification':False,'playback':False,'reference':refs}
        if args.probe:
            from build_sgb_score_peer import build as program_build
            candidates={f'{case}-{art}':native(args.probe.resolve(),bank(art,case)) for case in CASES for art in (63,127)}
            result.update(native=candidates,comparisons=compare(candidates,refs),program_sha256=hashlib.sha256(program_build()).hexdigest())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:p.error(str(error))
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
