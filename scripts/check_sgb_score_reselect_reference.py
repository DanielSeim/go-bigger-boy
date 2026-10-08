#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate actual prefix DSP setup writes and original reselection timing."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import subprocess
from build_sgb_score_reselect import build
from build_sgb_reselect_fixture import bank,build as fixture_build,expected
from check_sgb_score_sparse_reference import validate as sparse_validate,align as sparse_align,compare as sparse_compare,observe as sparse_observe,contract as sparse_contract,SCHEMA as SPARSE_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native,native_gates
from check_sgb_phrase_reference import run as phrase_run
from check_sgb_score_duet_reference import integer
from schedule_sgb_score import schedule

SCHEMA='gbb-spc-score-reselect-v1'
SETUP=(2,0x8F,0x6F,0xB8)
CASES={'reselect-1':1,'reselect-2':2}


def sparse_report(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA:
        raise ValueError('invalid instrument-reselection schema')
    return {**report,'schema':SPARSE_SCHEMA}


def validate(report):
    sparse_validate(sparse_report(report))
    writes=report.get('instrument_writes')
    if not isinstance(writes,list) or len(writes)>160: raise ValueError('invalid instrument write list')
    previous=0
    for write in writes:
        if not isinstance(write,dict) or not integer(write.get('half_cycle'),previous+1,report['completion_half_cycle']) or not integer(write.get('register'),0,127) or not integer(write.get('value'),0,255) or not integer(write.get('held_mask'),12,12):
            raise ValueError('invalid instrument write timestamp/held mask')
        previous=write['half_cycle']
    index,previous=0,0
    for event in report['events']:
        count=event.get('instrument_sets')
        if not integer(count,0,5): raise ValueError('invalid prefix instrument count')
        selected=writes[index:index+4*count];index+=4*count
        if len(selected)!=4*count or [(write['register'],write['value']) for write in selected]!=[(16*event['channel']+4+offset,value) for _ in range(count) for offset,value in enumerate(SETUP)] or any(not previous<write['half_cycle']<event['half_cycle'] for write in selected):
            raise ValueError('actual instrument setup writes differ from prefix count/voice/order')
        previous=event['half_cycle']
    if index!=len(writes) or (report['status']!=2 and writes):
        raise ValueError('unbound or rejected instrument writes')


def align(report,data):
    validate(report);sparse_align(sparse_report(report),data)
    expected_counts=[]
    for pattern in schedule(data,int.from_bytes(data[:2],'little'))['patterns']:
        import struct
        isolated=data+struct.pack('<HH',pattern['address'],0)
        counts={2:0,3:0}
        for event in schedule(isolated,0x2B00+len(data))['events']:
            if event['kind']=='instrument': counts[event['channel']]+=1
            elif event['kind'] in ('note','rest'):
                expected_counts.append(counts[event['channel']]);counts[event['channel']]=0
    if [event['instrument_sets'] for event in report['events']]!=expected_counts:
        raise ValueError('native instrument counts differ from raw score controls')


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=build,validator=validate,output_bound=131072)


def write_groups(case):
    count=CASES[case]
    def voice(channel): return [(16*channel+4+i,value) for i,value in enumerate(SETUP)]
    return [voice(2)+voice(3),[],voice(2)*count,[],voice(3)*count,[],voice(2)*count+voice(3)*count,[]]


def observe(source,case):
    text=source.read(16*1024*1024+1)
    if len(text)>16*1024*1024: raise ValueError('reselection trace exceeds byte bound')
    result=sparse_observe(io.StringIO(text),'transitions')
    groups,current=[],[]
    for row in csv.DictReader(io.StringIO(text)):
        if row['kind']!='D':continue
        address,value=(int(row[key]) for key in ('address','value'))
        if address in (0x24,0x25,0x26,0x27,0x34,0x35,0x36,0x37):current.append([address,value])
        if address==0x4C and value:groups.append(current);current=[]
    result['instrument_writes_before_onsets']=groups
    contract(result,case)
    return result


def contract(result,case):
    sparse_contract(result,'transitions')
    groups=result.get('instrument_writes_before_onsets');wanted=write_groups(case)
    if not isinstance(groups,list) or len(groups)!=8:raise ValueError('invalid original instrument-write groups')
    for index,(actual,target) in enumerate(zip(groups,wanted)):
        if not isinstance(actual,list) or any(not isinstance(pair,list) or len(pair)!=2 or any(type(value) is not int for value in pair) for pair in actual):
            raise ValueError('invalid original instrument write types')
        normalized=[tuple(pair) for pair in actual]
        if (normalized[-len(target):] if index==0 else normalized)!=target:
            raise ValueError('original instrument reselection register sequence differs')


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:fixture_build(art,name),
                      observer=lambda source:observe(source,case),contract=contract,pattern_durations=None)
    result.update(articulation=art,instruction_limit=8000000)
    return result


def compare(candidates,references):
    if not isinstance(candidates,dict):raise ValueError('invalid reselection candidates')
    keys={f'{case}-{art}' for case in CASES for art in (63,127)}
    if set(candidates)!=keys:
        for report in candidates.values():validate(report)
        return sparse_compare({key:sparse_report(report) for key,report in candidates.items()},references)
    if not isinstance(references,list) or len(references)!=8:raise ValueError('require both reselection cases/articulations/models')
    seen,comparisons=set(),[]
    for reference in references:
        if not isinstance(reference,dict):raise ValueError('invalid reselection reference')
        case,art,model=(reference.get(key) for key in ('case','articulation','model'))
        if case not in CASES or type(art) is not int or art not in (63,127) or model not in ('sgb','sgb2') or (case,art,model) in seen:
            raise ValueError('invalid/duplicate reselection reference')
        seen.add((case,art,model));contract(reference,case)
        report=candidates[f'{case}-{art}'];align(report,bank(art,case))
        for edge,target in zip(report['keyons'],expected(case)):
            if edge['mask']!=target['mask'] or any(edge['pitches'][voice['voice']-2]!=voice['pitch'] or edge['volumes'][voice['voice']-2]!=voice['volumes'] for voice in target['voices']):
                raise ValueError('native reselection onset differs')
        native=native_gates(report);original=reference['note_gates']
        differences=[abs(a['gate_spc_cycles']-b['gate_spc_cycles']) for a,b in zip(native,original)]
        if len(native)!=12 or any(a['voice']!=b['voice'] or a['cause']!=1 for a,b in zip(native,original)) or any(value>4096 for value in differences):
            raise ValueError('reselection gate outside existing allowance')
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        if len(intervals)!=7 or any(abs(a-b)>4096 for a,b in zip(intervals,reference['onset_intervals_spc_cycles'])):
            raise ValueError('reselection onset outside existing allowance')
        comparisons.append({'case':case,'articulation':art,'model':model,'absolute_gate_difference_spc_cycles':differences,
                            'native_DSP_intervals_spc_cycles':intervals,'reference_DSP_intervals_spc_cycles':reference['onset_intervals_spc_cycles']})
    for case in CASES:
        for art in (63,127):
            a,b=[r for r in references if r['case']==case and r['articulation']==art]
            if a['keyons']!=b['keyons'] or a['instrument_writes_before_onsets']!=b['instrument_writes_before_onsets'] or any(abs(x-y)>2048 for x,y in zip(a['onset_intervals_spc_cycles'],b['onset_intervals_spc_cycles'])):
                raise ValueError('reselection original models disagree')
    return comparisons


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--trace',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        candidates={f'{case}-{art}':native(args.probe.resolve(),bank(art,case)) for case in CASES for art in (127,63)}
        references=[run(args.trace.resolve(),args.firmware_dir.resolve(),model,art,case) for case in CASES for art in (127,63) for model in ('sgb','sgb2')]
        comparisons=compare(candidates,references)
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps({'schema':'gbb-score-reselect-reference-v1','qualification':False,'playback':False,
                      'program_sha256':hashlib.sha256(build()).hexdigest(),'native':candidates,'reference':references,'comparisons':comparisons},indent=2))


if __name__=='__main__':main()
