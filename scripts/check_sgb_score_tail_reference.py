#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded DSP comparisons and clipping diagnostics for trailing mix controls."""
import argparse
import csv
import io
import json
import hashlib
import subprocess
from pathlib import Path
from build_sgb_score_tail_fixture import bank,build,expected,CASES,QUALIFIED_CASES
from check_sgb_score_sparse_reference import onsets,onset_contract
from check_sgb_score_reselect_reference import validate as reselect_validate,SCHEMA as RESELECT_SCHEMA
from check_sgb_score_polygate_reference import align as gate_align,native as gate_native,native_gates
from check_sgb_score_chromatic_reference import PITCH
from check_sgb_score_mix_reference import selected
from schedule_sgb_score import schedule
from check_sgb_phrase_reference import run as phrase_run


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
    onset_contract(result,case,expected_onsets=expected(case))
    if 'note_gates' not in result:return
    from check_sgb_score_duet_reference import integer
    selected=int(case[-1]);clipped=case.startswith('clip') and bool(result.get('unreleased_retriggers'))
    if 'articulation' in result and case.startswith('clip') and clipped!=(result['articulation']==127):raise ValueError('clipped release behavior differs from articulation contract')
    wanted=[(index,voice['voice']) for index,edge in enumerate(expected(case)) for voice in edge['voices'] if not (clipped and index==1 and voice['voice']!=selected)]
    gates=result.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(wanted) or any(not isinstance(gate,dict) or type(gate.get('onset_index')) is not int or type(gate.get('voice')) is not int or (gate['onset_index'],gate['voice'])!=target or gate.get('release_kind')!='keyoff' or not integer(gate.get('gate_spc_cycles'),1,200000) for gate,target in zip(gates,wanted)):
        raise ValueError('tail gate observations differ from directly observed contract')
    retriggers=result.get('unreleased_retriggers')
    if clipped:
        voice=5-selected;index=4 if voice==3 else 2
        if not isinstance(retriggers,list) or len(retriggers)!=1 or not isinstance(retriggers[0],dict) or retriggers[0]!={**retriggers[0],'voice':voice,'onset_index':index,'previous_onset_index':1} or any(type(retriggers[0].get(key)) is not int for key in ('voice','onset_index','previous_onset_index')) or not integer(retriggers[0].get('interval_spc_cycles'),40000,300000):
            raise ValueError('clipped-peer unreleased retrigger differs')
    elif retriggers!=[]:raise ValueError('complete-ending fixture retriggered without release')
    if result.get('unreleased_final_voices')!=[]:raise ValueError('tail fixture ended with unresolved voice')


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:build(art,name),observer=lambda source:observe(source,case),contract=contract,pattern_durations=None)
    result.update(articulation=art,instruction_limit=8000000)
    contract(result,case)
    return result


SCHEMA='gbb-spc-score-tail-v1'


def validate(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA:raise ValueError('invalid trailing-control schema')
    reselect_validate({**report,'schema':RESELECT_SCHEMA})


def align(report,data):
    validate(report)
    gate_align(report,data,schema=SCHEMA,max_events=64,max_ticks=2032,pitch_table=PITCH,
               isolated_voices=False,pattern_ticks=report['pattern_ticks'],sparse=True,inherit_timing=True,end_priority=True)
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
    from build_sgb_score_tail import build
    return gate_native(probe,data,tempo,builder=build,validator=validate,output_bound=131072)


def compare(candidates,references,art):
    if type(art) is not int or art not in (63,127) or not isinstance(candidates,dict) or set(candidates)!=set(QUALIFIED_CASES) or not isinstance(references,list) or len(references)!=8:raise ValueError('require four complete-ending cases on both models')
    seen=set();comparisons=[]
    for reference in references:
        if not isinstance(reference,dict):raise ValueError('invalid trailing reference')
        case,model=reference.get('case'),reference.get('model')
        if case not in QUALIFIED_CASES or model not in ('sgb','sgb2') or type(reference.get('articulation')) is not int or reference['articulation']!=art or (case,model) in seen:raise ValueError('invalid or duplicate trailing reference')
        seen.add((case,model));contract(reference,case)
        report=candidates[case];align(report,bank(art,case))
        for edge,target in zip(report['keyons'],expected(case)):
            if edge['mask']!=target['mask'] or any(edge['pitches'][voice['voice']-2]!=voice['pitch'] or edge['volumes'][voice['voice']-2]!=voice['volumes'] for voice in target['voices']):raise ValueError('native trailing onset differs')
        gates=native_gates(report);original=reference['note_gates']
        differences=[abs(a['gate_spc_cycles']-b['gate_spc_cycles']) for a,b in zip(gates,original)]
        if len(gates)!=12 or any(a['voice']!=b['voice'] or a['cause']!=1 for a,b in zip(gates,original)) or any(v>4096 for v in differences):raise ValueError('trailing gate exceeds retained allowance')
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        if len(intervals)!=7 or any(abs(a-b)>4096 for a,b in zip(intervals,reference['onset_intervals_spc_cycles'])):raise ValueError('trailing onset exceeds retained allowance')
        comparisons.append({'case':case,'model':model,'articulation':art,'absolute_gate_difference_spc_cycles':differences,'native_DSP_intervals_spc_cycles':intervals,'reference_DSP_intervals_spc_cycles':reference['onset_intervals_spc_cycles']})
    for case in QUALIFIED_CASES:
        a,b=[r for r in references if r['case']==case]
        if a['keyons']!=b['keyons'] or any(abs(x-y)>2048 for x,y in zip(a['onset_intervals_spc_cycles'],b['onset_intervals_spc_cycles'])):raise ValueError('original tail models differ')
    return comparisons


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--trace',type=Path,required=True);parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--probe',type=Path)
    parser.add_argument('--case',choices=CASES,action='append');parser.add_argument('--articulation',type=int,choices=(63,127),default=127)
    args=parser.parse_args()
    try:
        refs=[]
        for case in (args.case or CASES):
            for model in ('sgb','sgb2'):
                result=run(args.trace.resolve(),args.firmware_dir.resolve(),model,args.articulation,case)
                refs.append(result)
        result={'schema':'gbb-score-tail-reference-v1','qualification':False,'playback':False,'reference':refs}
        if args.probe:
            from build_sgb_score_tail import build as program_build
            candidates={case:native(args.probe.resolve(),bank(args.articulation,case)) for case in QUALIFIED_CASES}
            result.update(program_sha256=hashlib.sha256(program_build()).hexdigest(),native=candidates,comparisons=compare(candidates,[r for r in refs if r['case'] in QUALIFIED_CASES],args.articulation))
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
