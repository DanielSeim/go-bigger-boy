#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sparse pattern lifecycle and strict opaque original DSP comparisons."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
from build_sgb_score_sparse import build
from check_sgb_score_list_reference import compare as list_compare, SCHEMA as LIST_SCHEMA
from check_sgb_score_envelope_reference import validate as envelope_validate, align as envelope_align, SCHEMA as ENVELOPE_SCHEMA
from check_sgb_score_polygate_reference import native as gate_native, native_gates, observe as gate_observe
from check_sgb_score_duet_reference import integer
from check_sgb_phrase_reference import run as phrase_run, MAX_ROWS
from check_sgb_chromatic_reference import PITCHES
from schedule_sgb_score import schedule
from build_sgb_sparse_fixture import CASES,bank,build as fixture_build,expected

SCHEMA='gbb-spc-score-sparse-v1'


def list_report(report):
    if not isinstance(report,dict) or report.get('schema')!=SCHEMA or report.get('source_unmodified') is not True or report.get('cache_guards_equal') is not True:
        raise ValueError('invalid sparse schema/source/cache preservation')
    return {**report,'schema':LIST_SCHEMA}


def validate(report):
    list_report(report)
    ticks,masks=report.get('pattern_ticks'),report.get('pattern_masks')
    if not isinstance(ticks,list) or not isinstance(masks,list) or len(ticks)!=len(masks) or any(type(mask) is not int or mask not in (4,8,12) for mask in masks):
        raise ValueError('invalid sparse pattern masks')
    envelope_validate({**report,'schema':ENVELOPE_SCHEMA},max_events=64,pattern_ticks=ticks,sparse=True)
    for index,(tick,mask) in enumerate(zip(ticks,masks)):
        end=ticks[index+1] if index+1<len(ticks) else report['end_tick']
        events=[event for event in report['events'] if tick<=event['tick']<end]
        if sum(1<<event['channel'] for event in events if event['tick']==tick)!=mask or any(not mask & (1<<event['channel']) for event in events):
            raise ValueError('sparse log emitted missing/inactive track')


def align(report,data):
    validate(report)
    envelope_align({**report,'schema':ENVELOPE_SCHEMA},data,max_events=64,pattern_ticks=report['pattern_ticks'],sparse=True)
    wanted=schedule(data,int.from_bytes(data[:2],'little'))
    if report['pattern_masks']!=[sum(1<<channel for channel in pattern['channels']) for pattern in wanted['patterns']]:
        raise ValueError('sparse active masks differ from independent scheduler')


def native(probe,data,tempo=96):
    return gate_native(probe,data,tempo,builder=build,validator=validate,output_bound=131072)


def onset_contract(result,case, *, expected_onsets=None):
    if not isinstance(result,dict) or result.get('keyons')!=(expected(case) if expected_onsets is None else expected_onsets):
        raise ValueError('sparse original setup/onsets differ from owned contract')
    for edge in result['keyons']:
        if type(edge['mask']) is not int or any(type(value) is not int for voice in edge['voices'] for key,value in voice.items() if key!='volumes') or any(type(value) is not int for voice in edge['voices'] for value in voice['volumes']):
            raise ValueError('sparse original setup must contain integers')
    intervals=result.get('onset_intervals_spc_cycles')
    if not isinstance(intervals,list) or len(intervals)!=7 or any(not integer(value,84000,92000) for value in intervals):
        raise ValueError('sparse original onset outside existing bounds')


def onsets(source,case, *, expected_onsets=None, onset_validator=None):
    reader=csv.DictReader(source)
    if reader.fieldnames!=['kind','master_clock','spc_cycle','pcm_sample','address','value']:
        raise ValueError('invalid sparse trace header')
    registers,edges,cycles,previous={},[],[],-1
    for count,row in enumerate(reader,1):
        if count>=MAX_ROWS: raise ValueError('sparse trace reached row bound')
        if row['kind']!='D': continue
        try: address,value,cycle=(int(row[key]) for key in ('address','value','spc_cycle'))
        except (TypeError,ValueError): raise ValueError('invalid sparse DSP event') from None
        if None in row or any(value is None for value in row.values()) or not 0<=address<128 or not 0<=value<256 or cycle<previous:
            raise ValueError('invalid/unordered sparse DSP event')
        previous=cycle;registers[address]=value
        if address!=0x4C or not value: continue
        if value not in (4,8,12) or len(edges)>=8 or registers.get(0x3D,12)&12:
            raise ValueError('unexpected sparse onset/voice/noise')
        voices=[]
        for voice in (2,3):
            if not value&(1<<voice): continue
            base=16*voice
            if any(base+i not in registers for i in range(8)): raise ValueError('incomplete sparse voice setup')
            voices.append({'voice':voice,'pitch':registers[base+2]|registers[base+3]<<8,
                           'volumes':[registers[base],registers[base+1]],'srcn':registers[base+4],
                           'adsr1':registers[base+5],'adsr2':registers[base+6],'gain':registers[base+7]})
        edges.append({'mask':value,'voices':voices});cycles.append(cycle)
    result={'keyons':edges,'onset_intervals_spc_cycles':[b-a for a,b in zip(cycles,cycles[1:])]}
    if onset_validator is None:
        onset_contract(result,case,expected_onsets=expected_onsets)
    else:
        onset_validator(result,case)
    return result


def observe(source,case):
    return gate_observe(source,onset_observer=lambda trace:onsets(trace,case),
                        expected_notes=sum(len(edge['voices']) for edge in expected(case)),handoff_note_count=None)


def contract(result,case):
    onset_contract(result,case)
    voices=[voice['voice'] for edge in expected(case) for voice in edge['voices']]
    gates=result.get('note_gates')
    if not isinstance(gates,list) or len(gates)!=len(voices) or any(not isinstance(gate,dict) or type(gate.get('voice')) is not int or gate['voice']!=voice or gate.get('release_kind')!='keyoff' or not integer(gate.get('gate_spc_cycles'),1,200000) for gate,voice in zip(gates,voices)):
        raise ValueError('sparse reference requires directly observed releases')


def run(trace,firmware_dir,model,art,case):
    result=phrase_run(trace,firmware_dir,model,case,fixture_builder=lambda name:fixture_build(art,name),
                      observer=lambda source:observe(source,case),contract=contract,pattern_durations=None)
    result.update(articulation=art,instruction_limit=8000000)
    return result


def compare(candidates,references):
    keys={f'{case}-{art}' for case in CASES for art in (63,127)}
    if not isinstance(candidates,dict): raise ValueError('invalid sparse candidates')
    if set(candidates)!=keys:
        for report in candidates.values(): validate(report)
        return list_compare({key:list_report(value) for key,value in candidates.items()},references)
    if not isinstance(references,list) or len(references)!=12: raise ValueError('require sparse cases/articulations/models')
    seen,comparisons=set(),[]
    for reference in references:
        if not isinstance(reference,dict): raise ValueError('invalid sparse reference')
        case,art,model=(reference.get(key) for key in ('case','articulation','model'))
        if case not in CASES or type(art) is not int or art not in (63,127) or model not in ('sgb','sgb2') or (case,art,model) in seen:
            raise ValueError('invalid/duplicate sparse reference')
        seen.add((case,art,model));contract(reference,case)
        report=candidates[f'{case}-{art}'];align(report,bank(art,case))
        if len(report['keyons'])!=8: raise ValueError('sparse native onset count differs')
        for edge,wanted in zip(report['keyons'],expected(case)):
            if edge['mask']!=wanted['mask'] or any(edge['pitches'][voice['voice']-2]!=voice['pitch'] or edge['volumes'][voice['voice']-2]!=voice['volumes'] for voice in wanted['voices']):
                raise ValueError('sparse native pitch/volume/voice differs')
        actual=native_gates(report);original=reference['note_gates']
        differences=[abs(a['gate_spc_cycles']-b['gate_spc_cycles']) for a,b in zip(actual,original)]
        if len(actual)!=len(original) or any(a['voice']!=b['voice'] or a['cause']!=1 for a,b in zip(actual,original)) or any(value>4096 for value in differences):
            raise ValueError('sparse gate outside existing allowance')
        intervals=[(b['half_cycle']-a['half_cycle'])/2 for a,b in zip(report['keyons'],report['keyons'][1:])]
        if any(abs(a-b)>4096 for a,b in zip(intervals,reference['onset_intervals_spc_cycles'])):
            raise ValueError('sparse onset outside existing allowance')
        comparisons.append({'case':case,'articulation':art,'model':model,'absolute_gate_difference_spc_cycles':differences,
                            'native_DSP_intervals_spc_cycles':intervals,'reference_DSP_intervals_spc_cycles':reference['onset_intervals_spc_cycles']})
    for case in CASES:
        for art in (63,127):
            a,b=[reference for reference in references if reference['case']==case and reference['articulation']==art]
            if a['keyons']!=b['keyons'] or any(abs(x-y)>2048 for x,y in zip(a['onset_intervals_spc_cycles'],b['onset_intervals_spc_cycles'])):
                raise ValueError('sparse original models disagree')
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
    except (OSError,ValueError,subprocess.TimeoutExpired) as error: parser.error(str(error))
    print(json.dumps({'schema':'gbb-score-sparse-reference-v1','qualification':False,'playback':False,
                      'program_sha256':hashlib.sha256(build()).hexdigest(),'native':candidates,'reference':references,'comparisons':comparisons},indent=2))


if __name__=='__main__': main()
